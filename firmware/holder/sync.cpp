// sync.cpp — see sync.h. Requires ArduinoJson 7 (Library Manager).
#include "sync.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

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

String url(const char* path) { return String(SERVER_URL) + path; }

void make_client(WiFiClientSecure& c) {
#if INSECURE_TLS
  c.setInsecure();
#endif
  c.setTimeout(15);
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

// Download a URL to a LittleFS path. Content-addressed names mean a file that
// exists is already correct, so this is skipped when it is present.
bool download(const String& u, const char* path) {
  if (LittleFS.exists(path)) return true;
  WiFiClientSecure client; make_client(client);
  HTTPClient http; http.begin(client, u);
  http.addHeader("X-Holder-Key", HOLDER_KEY);
  int code = http.GET();
  if (code != 200) { Serial.printf("[sync] GET %s -> %d\n", u.c_str(), code); http.end(); return false; }
  String tmp = String(path) + ".part";
  File f = LittleFS.open(tmp, "w");
  if (!f) { http.end(); return false; }
  int written = http.writeToStream(&f);
  f.close(); http.end();
  if (written <= 0) { LittleFS.remove(tmp); return false; }
  LittleFS.rename(tmp, path);
  return true;
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

bool fetch_bulletin(const char* version_hint) {
  WiFiClientSecure client; make_client(client);
  HTTPClient http; http.begin(client, url("/api/holder/" HOLDER_ID "/bulletin"));
  http.addHeader("X-Holder-Key", HOLDER_KEY);
  int code = http.GET();
  if (code != 200) { Serial.printf("[sync] bulletin -> %d\n", code); http.end(); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { Serial.printf("[sync] bulletin json: %s\n", err.c_str()); return false; }

  Bulletin b;
  strlcpy(b.version, doc["version"] | version_hint, sizeof b.version);
  strlcpy(b.source,  doc["source"]  | "news", sizeof b.source);
  strlcpy(b.source_label, doc["source_label"] | "", sizeof b.source_label);

  auto sysclip = [&](const char* key, char* dst) {
    const char* file = doc["system"][key] | "";
    if (!*file) { dst[0] = 0; return; }
    snprintf(dst, 40, "/m/%s", file);
    download(url("/media/") + file, dst);
  };
  sysclip("switch",   b.sys_switch);
  sysclip("low_paper",b.sys_low);
  sysclip("new_roll", b.sys_newroll);
  sysclip("offline",  b.sys_offline);
  sysclip("empty",    b.sys_empty);

  for (JsonObject it : doc["items"].as<JsonArray>()) {
    if (b.count >= 40) break;
    BulletinItem& x = b.items[b.count];
    const char* file = it["file"] | "";
    if (!*file) continue;
    strlcpy(x.id, it["id"] | "", sizeof x.id);
    snprintf(x.file, sizeof x.file, "/m/%s", file);
    x.duration_ms = it["duration_ms"] | 4000;
    if (!download(url("/media/") + file, x.file)) continue;   // skip what we could not get
    ++b.count;
  }
  sweep(b);
  Serial.printf("[sync] bulletin %s: %u items from %s\n", b.version, b.count, b.source);

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_pending = b;
  g_pending_ready = true;
  g_pending_switch = strcmp(b.source, g_current_source) != 0 && g_current_source[0] != 0;
  strlcpy(g_current_version, b.version, sizeof g_current_version);
  strlcpy(g_current_source, b.source, sizeof g_current_source);
  xSemaphoreGive(g_mutex);
  return true;
}

// POST telemetry; returns the poll interval the server asks for (0 on error).
uint32_t heartbeat() {
  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["uptime_s"] = millis() / 1000;
  doc["rssi"] = WiFi.RSSI();
  doc["sheets_total"] = g_status->sheets_total;
  doc["sheets_this_roll"] = g_status->sheets_this_roll;
  doc["roll_sheets"] = ROLL_SHEETS;
  doc["last_pull_s_ago"] = g_status->last_pull_ms ? (millis() - g_status->last_pull_ms) / 1000 : -1;
  doc["queued"] = g_status->queued;
  doc["playing"] = g_status->playing;
  doc["bulletin"] = g_current_version;
  doc["source"] = g_current_source;
  doc["mains"] = MAINS_POWERED ? true : false;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  JsonArray pl = doc["played"].to<JsonArray>();
  for (uint8_t i = 0; i < g_played_n; ++i) pl.add(g_played[i]);
  uint8_t sent = g_played_n;
  xSemaphoreGive(g_mutex);
  size_t total = 0, used = 0; total = LittleFS.totalBytes(); used = LittleFS.usedBytes();
  doc["fs_free_kb"] = (total - used) / 1024;
  String body; serializeJson(doc, body);

  WiFiClientSecure client; make_client(client);
  HTTPClient http; http.begin(client, url("/api/holder/" HOLDER_ID "/heartbeat"));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Holder-Key", HOLDER_KEY);
  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[sync] heartbeat -> %d\n", code);
    g_status->last_sync_error = code; http.end(); return 0;
  }
  JsonDocument r; deserializeJson(r, http.getStream()); http.end();
  g_status->last_sync_error = 0;
  g_status->last_sync_ms = millis();
  xSemaphoreTake(g_mutex, portMAX_DELAY);          // drop what the server now knows about
  if (sent <= g_played_n) { memmove(g_played, g_played + sent, (g_played_n - sent) * 24); g_played_n -= sent; }
  xSemaphoreGive(g_mutex);
  g_status->online = true;

  const char* want_version = r["bulletin"] | "";
  const char* want_source  = r["source"] | "";
  if (strcmp(want_version, g_current_version) != 0 || strcmp(want_source, g_current_source) != 0) {
    Serial.printf("[sync] server has %s (%s), we have %s (%s) -> fetching\n",
                  want_version, want_source, g_current_version, g_current_source);
    fetch_bulletin(want_version);
  }
  return r["poll_s"] | (uint32_t)(MAINS_POWERED ? POLL_MAINS_S : POLL_BATTERY_S);
}

void task(void*) {
  for (;;) {
    uint32_t wait_s = MAINS_POWERED ? POLL_MAINS_S : POLL_BATTERY_S;
    if (ensure_wifi()) {
      g_status->rssi = WiFi.RSSI();
      uint32_t p = heartbeat();
      if (p) wait_s = p; else g_status->online = false;
    } else {
      g_status->online = false;
      wait_s = 30;
    }
    for (uint32_t i = 0; i < wait_s * 10 && !g_poke; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    g_poke = false;
  }
}

}  // namespace

namespace sync {

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
    }
    f.close();
  }
  xTaskCreatePinnedToCore(task, "sync", 12288, nullptr, 1, nullptr, 0);
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
void note_played(const char* item_id) {
  if (!item_id || !*item_id) return;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (g_played_n < 16) { strlcpy(g_played[g_played_n], item_id, 24); ++g_played_n; }
  xSemaphoreGive(g_mutex);
}

}  // namespace sync
