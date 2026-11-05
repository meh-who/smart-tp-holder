// sync.cpp — see sync.h. Requires ArduinoJson 7 (Library Manager).
#include "sync.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_system.h>

namespace {

HolderStatus*      g_status = nullptr;
SemaphoreHandle_t  g_mutex;
Bulletin           g_pending;          // built by the task, handed to the main loop
bool               g_pending_ready = false;
bool               g_pending_switch = false;
char               g_current_version[40] = "";
char               g_current_source[16]  = "";
volatile bool      g_poke = false;
char               g_played[16][24];       // ids spoken since the last heartbeat
volatile uint8_t   g_played_n = 0;
double             g_page_open_ts = 0;      // server timestamp of the last companion-page open
volatile bool      g_phone_tapped = false;
double             g_roll_reset_seen = 0;   // last "New roll" request applied (server timestamp)
double             g_roll_reset_pending = 0;

// One kept-alive TLS connection carries everything: heartbeats, the bulletin
// and every MP3. The handshake is the slow, CPU-heavy part on the S2 (and the
// part that can stall), so it happens once rather than once per file, and while
// the connection is open a heartbeat costs a few milliseconds and can go out
// mid-sentence. Heartbeats also keep going while a bulletin downloads, so the
// page never freezes.
WiFiClientSecure   g_tls;
HTTPClient         g_http;
Bulletin           g_build;                    // being downloaded (too big for the task's stack)
char               g_want_version[40] = "";    // the bulletin the server last advertised
char               g_fetching_version[40] = "";
bool               g_fetch_wanted = false;
bool               g_fetching = false;
uint32_t           g_fetch_not_before = 0;
uint8_t            g_fetch_attempts = 0;       // failed or incomplete tries at g_want_version
uint8_t            g_items_cached = 0;         // items in the last bulletin handed over
uint32_t           g_last_beat_ms = 0;
uint32_t           g_next_beat_ms = 0;         // 0: beat as soon as WiFi is up
bool               g_in_beat = false;
uint32_t           g_fs_free_kb = 0;           // refreshed only while silent (it walks the filesystem)
bool               g_fs_known = false;

String url(const String& path) { return String(SERVER_URL) + path; }

// Why the chip last restarted, sent with every heartbeat: a holder that keeps
// crashing or browning out (amp + WiFi on a weak USB supply) shows up remotely.
const char* reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:   return "watchdog (interrupt)";
    case ESP_RST_TASK_WDT:  return "watchdog (task)";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    default:                return "other";
  }
}

bool quiet() {
#if CONFIG_FREERTOS_UNICORE || defined(CONFIG_IDF_TARGET_ESP32S2)
  return !(g_status && g_status->playing);
#else
  return true;
#endif
}

void service_beat();

// On single-core chips (ESP32-S2) the network shares the CPU with the MP3
// decoder, and writing a download to flash briefly stalls everything, so new
// connections and downloads wait for the current clip to finish (up to a
// minute). Heartbeats keep going meanwhile over the open connection.
void wait_quiet() {
  for (int i = 0; i < 600 && !quiet(); ++i) { service_beat(); vTaskDelay(pdMS_TO_TICKS(100)); }
}

void make_client(WiFiClientSecure& c) {
#if INSECURE_TLS
  c.setInsecure();
#endif
  c.setTimeout(15);
  c.setHandshakeTimeout(15);   // the library default is 120 s, long enough to look offline
}

bool ensure_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("[sync] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(MAINS_POWERED ? false : true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) delay(500);
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("[sync] %s\n", ok ? WiFi.localIP().toString().c_str() : "no wifi");
  return ok;
}

// One request over the shared connection. On 200 the body goes to `reply` or
// `sink`; any other answer's body is read and dropped so the connection stays
// usable. Returns the HTTP status, or a negative HTTPClient error, in which
// case the connection is closed and the next request opens a new one.
int send(const String& path, const String* post_body, String* reply, Stream* sink = nullptr) {
  int code = HTTPC_ERROR_CONNECTION_REFUSED;
  if (g_http.begin(g_tls, url(path))) {
    g_http.addHeader("X-Holder-Key", HOLDER_KEY);
    if (post_body) { g_http.addHeader("Content-Type", "application/json"); code = g_http.POST(*post_body); }
    else           code = g_http.GET();
    if (code == HTTP_CODE_OK) {
      if (sink) {
        int size = g_http.getSize();
        int n = g_http.writeToStream(sink);
        if (n < 0) code = n;
        else if (size > 0 && n != size) code = HTTPC_ERROR_CONNECTION_LOST;
      } else if (reply) {
        *reply = g_http.getString();
      }
    } else if (code > 0) {
      g_http.getString();                  // drain the error body
    }
  }
  if (code < 0) {
    char err[96] = "";
    g_tls.lastError(err, sizeof err);
    Serial.printf("[sync] %s -> %d %s | tls: %s | heap %u\n", path.c_str(), code,
                  HTTPClient::errorToString(code).c_str(), err, (unsigned)ESP.getFreeHeap());
  }
  g_http.end();                            // keeps the socket open when the server allows it
  if (code < 0) g_tls.stop();
  return code;
}

// ---- heartbeat ---------------------------------------------------------------
// The body: telemetry plus the item ids spoken since the last beat. `sent` is
// how many of those ids it carries, dropped once the server has them.
String beat_body(uint8_t& sent) {
  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["uptime_s"] = millis() / 1000;
  int32_t rssi = WiFi.RSSI();
  g_status->rssi = rssi;
  doc["rssi"] = rssi;
  doc["sheets_total"] = g_status->sheets_total;
  doc["sheets_this_roll"] = g_status->sheets_live;    // includes the pull in progress
  doc["roll_sheets"] = ROLL_SHEETS;
  doc["last_pull_s_ago"] = g_status->last_pull_ms ? (int32_t)((millis() - g_status->last_pull_ms) / 1000) : (int32_t)-1;  // signed: -1 = never
  doc["queued"] = g_status->queued;
  doc["playing"] = g_status->playing;
  doc["bulletin"] = g_current_version;
  doc["source"] = g_current_source;
  doc["items"] = g_items_cached;
  doc["fetching"] = g_fetching ? g_fetching_version : "";
  doc["fetch_attempts"] = g_fetch_attempts;
  doc["mains"] = MAINS_POWERED ? true : false;
  doc["roll_reset_seen"] = g_roll_reset_seen;
  doc["heap"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  doc["reset"] = reset_reason();
  doc["last_error"] = (int32_t)g_status->last_sync_error;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  JsonArray pl = doc["played"].to<JsonArray>();
  for (uint8_t i = 0; i < g_played_n; ++i) pl.add(g_played[i]);
  sent = g_played_n;
  xSemaphoreGive(g_mutex);
  // Free space walks the whole filesystem, which would hold up the MP3 reads,
  // so it is measured between clips and the last figure is sent otherwise.
  if (!g_status->playing) {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    g_fs_free_kb = (total - used) / 1024; g_fs_known = true;
  }
  if (g_fs_known) doc["fs_free_kb"] = g_fs_free_kb;
  String body; serializeJson(doc, body);
  return body;
}

void heartbeat() {
  bool warm = g_tls.connected();
  g_poke = false;                          // anything that changes after this gets its own beat
  g_last_beat_ms = millis();
  uint8_t sent = 0;
  String body = beat_body(sent), reply;
  int code = send("/api/holder/" HOLDER_ID "/heartbeat", &body, &reply);
  if (code != HTTP_CODE_OK) {
    if (code > 0) Serial.printf("[sync] heartbeat -> %d\n", code);
    g_status->last_sync_error = code;
    g_status->online = false;
    // A kept-alive connection the server had dropped: try again straight away
    // on a new one. Anything else: in five seconds.
    g_next_beat_ms = millis() + (warm && code < 0 ? 0 : 5000);
    return;
  }
  JsonDocument r; deserializeJson(r, reply);
  g_status->last_sync_error = 0;
  g_status->last_sync_ms = millis();
  g_status->online = true;
  xSemaphoreTake(g_mutex, portMAX_DELAY);          // drop what the server now knows about
  if (sent <= g_played_n) { memmove(g_played, g_played + sent, (g_played_n - sent) * 24); g_played_n -= sent; }
  xSemaphoreGive(g_mutex);
  uint32_t poll_s = r["poll_s"] | (uint32_t)(MAINS_POWERED ? POLL_MAINS_S : POLL_BATTERY_S);
  g_next_beat_ms = millis() + poll_s * 1000UL;

  double rr = r["roll_reset_ts"] | 0.0;
  if (rr > g_roll_reset_seen && rr > g_roll_reset_pending) g_roll_reset_pending = rr;
  double page_ts = r["page_open_ts"] | 0.0;
  if (page_ts > g_page_open_ts) {
    if (g_page_open_ts > 0) g_phone_tapped = true;   // not on the very first heartbeat after boot
    g_page_open_ts = page_ts;
  }
  // A different bulletin on the server: the task fetches it, between beats.
  const char* want_version = r["bulletin"] | "";
  const char* want_source  = r["source"] | "";
  if (*want_version) {
    bool differs = g_fetching ? strcmp(want_version, g_fetching_version) != 0
                              : (strcmp(want_version, g_current_version) != 0 || strcmp(want_source, g_current_source) != 0);
    if (differs) {
      if (strcmp(want_version, g_want_version) != 0) {
        Serial.printf("[sync] server has %s (%s), we have %s (%s) -> fetching\n",
                      want_version, want_source, g_current_version, g_current_source);
        strlcpy(g_want_version, want_version, sizeof g_want_version);
        g_fetch_attempts = 0; g_fetch_not_before = 0;
      }
      g_fetch_wanted = true;
    }
  }
}

// Beat if one is due (the poll interval is up, or a sheet was pulled), at most
// one per MIN_BEAT_MS. Called from the task loop, and between files and while
// waiting for quiet during a download, so a fetch never holds up the count.
void service_beat() {
  if (g_in_beat || !g_status) return;
  uint32_t now = millis();
  bool due = g_poke || (int32_t)(now - g_next_beat_ms) >= 0;
  if (!due || now - g_last_beat_ms < MIN_BEAT_MS) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!quiet() && !g_tls.connected()) return;      // a new handshake waits for the clip to end
  g_in_beat = true;
  heartbeat();
  g_in_beat = false;
}

// ---- bulletin + media ------------------------------------------------------------
// Download a file to LittleFS. Content-addressed names mean a file that exists
// is already correct, so this is skipped when it is present.
bool download(const char* file, const char* path) {
  if (LittleFS.exists(path)) return true;
  String tmp = String(path) + ".part";
  for (int attempt = 0; attempt < 2; ++attempt) {
    wait_quiet();
    service_beat();
    File f = LittleFS.open(tmp, "w");
    if (!f) { Serial.printf("[sync] can't write %s (filesystem full or not mounted?)\n", tmp.c_str()); return false; }
    int code = send(String("/media/") + file, nullptr, nullptr, &f);
    f.close();
    if (code == HTTP_CODE_OK) { LittleFS.rename(tmp, path); return true; }
    LittleFS.remove(tmp);
    if (code > 0) { Serial.printf("[sync] /media/%s -> %d\n", file, code); return false; }   // 404 and the like: don't retry
  }
  return false;
}

// Remove cached MP3s not referenced by the new bulletin so the 6 MB partition
// never fills. System clips are kept across sources.
void sweep(const Bulletin& b) {
  File dir = LittleFS.open("/m");
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String p = String("/m/") + f.name();
    bool keep = p.indexOf("sys_") >= 0;
    for (uint8_t i = 0; i < b.count && !keep; ++i) keep = p == b.items[i].file;
    if (!keep) { String del = p; f.close(); LittleFS.remove(del); }
  }
}

// Not got (all of) it: try again later, backing off from 10 s to 5 min.
void retry_fetch_later() {
  if (g_fetch_attempts < 250) ++g_fetch_attempts;
  uint32_t backoff_s = 10UL << (g_fetch_attempts > 5 ? 5 : g_fetch_attempts - 1);
  if (backoff_s > 300) backoff_s = 300;
  g_fetch_wanted = true;
  g_fetch_not_before = millis() + backoff_s * 1000UL;
}

void fetch_bulletin() {
  g_fetch_wanted = false;
  g_fetching = true;
  strlcpy(g_fetching_version, g_want_version, sizeof g_fetching_version);
  wait_quiet();
  String body;
  int code = send("/api/holder/" HOLDER_ID "/bulletin", nullptr, &body);
  if (code != HTTP_CODE_OK) {
    if (code > 0) Serial.printf("[sync] bulletin -> %d\n", code);
    g_fetching = false; retry_fetch_later(); return;
  }
  JsonDocument filter;                     // the item texts are for the page, not the holder
  filter["version"] = true; filter["source"] = true; filter["source_label"] = true; filter["system"] = true;
  filter["items"][0]["id"] = true; filter["items"][0]["file"] = true; filter["items"][0]["duration_ms"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  body = String();
  if (err) {
    Serial.printf("[sync] bulletin json: %s\n", err.c_str());
    g_fetching = false; retry_fetch_later(); return;
  }

  Bulletin& b = g_build;
  b = Bulletin();
  strlcpy(b.version, doc["version"] | g_fetching_version, sizeof b.version);
  strlcpy(b.source,  doc["source"]  | "news", sizeof b.source);
  strlcpy(b.source_label, doc["source_label"] | "", sizeof b.source_label);
  strlcpy(g_fetching_version, b.version, sizeof g_fetching_version);
  bool complete = true;

  auto sysclip = [&](const char* key, char* dst) {
    const char* file = doc["system"][key] | "";
    dst[0] = 0;
    if (!*file) return;
    char path[40]; snprintf(path, sizeof path, "/m/%s", file);
    if (download(file, path)) strlcpy(dst, path, 40); else complete = false;
  };
  sysclip("switch",   b.sys_switch);
  sysclip("low_paper",b.sys_low);
  sysclip("new_roll", b.sys_newroll);
  sysclip("offline",  b.sys_offline);
  sysclip("empty",    b.sys_empty);

  JsonArray items = doc["items"].as<JsonArray>();
  uint8_t listed = 0;
  for (JsonObject it : items) {
    if (b.count >= 40) break;
    BulletinItem& x = b.items[b.count];
    const char* file = it["file"] | "";
    if (!*file) continue;
    ++listed;
    strlcpy(x.id, it["id"] | "", sizeof x.id);
    snprintf(x.file, sizeof x.file, "/m/%s", file);
    x.duration_ms = it["duration_ms"] | 4000;
    if (!download(file, x.file)) { complete = false; continue; }   // speak what we have; fetch the rest later
    ++b.count;
  }
  sweep(b);
  Serial.printf("[sync] bulletin %s: %u of %u items from %s%s\n", b.version, b.count, listed, b.source,
                complete ? "" : " (incomplete, will retry)");

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_pending = b;
  g_pending_ready = true;
  g_pending_switch = strcmp(b.source, g_current_source) != 0 && g_current_source[0] != 0;
  strlcpy(g_current_version, b.version, sizeof g_current_version);
  strlcpy(g_current_source, b.source, sizeof g_current_source);
  xSemaphoreGive(g_mutex);
  g_items_cached = b.count;
  g_fetching = false;
  if (complete) g_fetch_attempts = 0; else retry_fetch_later();
  g_poke = true;                           // tell the page what we now have
}

void task(void*) {
  Serial.printf("[sync] last restart: %s\n", reset_reason());
  make_client(g_tls);
  g_http.setReuse(true);
  for (;;) {
    if (!ensure_wifi()) {
      g_status->online = false;
      g_tls.stop();
      vTaskDelay(pdMS_TO_TICKS(30000));
      continue;
    }
    service_beat();
    if (g_fetch_wanted && !g_fetching && (int32_t)(millis() - g_fetch_not_before) >= 0) fetch_bulletin();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

namespace net {

void begin(HolderStatus* status) {
  g_status = status;
  g_mutex = xSemaphoreCreateMutex();
  LittleFS.mkdir("/m");
  // Load the last bulletin from flash so the holder speaks before WiFi is up.
  File f = LittleFS.open("/bulletin.json", "r");
  if (f) {
    JsonDocument doc;
    if (!deserializeJson(doc, f)) {
      strlcpy(g_current_version, doc["version"] | "", sizeof g_current_version);
      strlcpy(g_current_source,  doc["source"]  | "", sizeof g_current_source);
      for (JsonObject it : doc["items"].as<JsonArray>()) if (LittleFS.exists(it["file"] | "/none")) ++g_items_cached;
    }
    f.close();
  }
  xTaskCreatePinnedToCore(task, "sync", 16384, nullptr, 1, nullptr, 0);
}

bool take_bulletin(Bulletin& out, bool& source_changed) {
  if (!g_pending_ready) return false;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  out = g_pending;
  source_changed = g_pending_switch;
  g_pending_ready = false; g_pending_switch = false;
  xSemaphoreGive(g_mutex);
  // Persist a tiny copy so the next boot knows what it has.
  File f = LittleFS.open("/bulletin.json", "w");
  if (f) {
    JsonDocument doc;
    doc["version"] = out.version; doc["source"] = out.source; doc["source_label"] = out.source_label;
    doc["sys_switch"] = out.sys_switch; doc["sys_low"] = out.sys_low; doc["sys_newroll"] = out.sys_newroll;
    doc["sys_offline"] = out.sys_offline; doc["sys_empty"] = out.sys_empty;
    JsonArray arr = doc["items"].to<JsonArray>();
    for (uint8_t i = 0; i < out.count; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = out.items[i].id; o["file"] = out.items[i].file; o["duration_ms"] = out.items[i].duration_ms;
    }
    serializeJson(doc, f); f.close();
  }
  return true;
}

bool load_cached(Bulletin& out) {
  File f = LittleFS.open("/bulletin.json", "r");
  if (!f) return false;
  JsonDocument doc;
  bool ok = !deserializeJson(doc, f);
  f.close();
  if (!ok) return false;
  Bulletin b;
  strlcpy(b.version, doc["version"] | "", sizeof b.version);
  strlcpy(b.source,  doc["source"]  | "", sizeof b.source);
  strlcpy(b.source_label, doc["source_label"] | "", sizeof b.source_label);
  strlcpy(b.sys_switch,  doc["sys_switch"]  | "", sizeof b.sys_switch);
  strlcpy(b.sys_low,     doc["sys_low"]     | "", sizeof b.sys_low);
  strlcpy(b.sys_newroll, doc["sys_newroll"] | "", sizeof b.sys_newroll);
  strlcpy(b.sys_offline, doc["sys_offline"] | "", sizeof b.sys_offline);
  strlcpy(b.sys_empty,   doc["sys_empty"]   | "", sizeof b.sys_empty);
  for (JsonObject it : doc["items"].as<JsonArray>()) {
    if (b.count >= 40) break;
    BulletinItem& x = b.items[b.count];
    strlcpy(x.id, it["id"] | "", sizeof x.id);
    strlcpy(x.file, it["file"] | "", sizeof x.file);
    x.duration_ms = it["duration_ms"] | 4000;
    if (LittleFS.exists(x.file)) ++b.count;
  }
  out = b;
  return b.count > 0;
}

bool online() { return g_status && g_status->online; }
void request_now() { g_poke = true; }
bool phone_tapped() { bool t = g_phone_tapped; g_phone_tapped = false; return t; }
void set_roll_reset_seen(double ts) { g_roll_reset_seen = ts; }
bool take_roll_reset(double& ts) {
  if (g_roll_reset_pending <= g_roll_reset_seen) return false;
  ts = g_roll_reset_pending;
  g_roll_reset_seen = ts;            // reported in the next heartbeat, so the page stops showing "pending"
  return true;
}
void note_played(const char* item_id) {
  if (!item_id || !*item_id) return;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (g_played_n < 16) { strlcpy(g_played[g_played_n], item_id, 24); ++g_played_n; }
  xSemaphoreGive(g_mutex);
}

}  // namespace net
