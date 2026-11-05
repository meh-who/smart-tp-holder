// sync_sim.cpp — runs the real firmware/holder/sync.cpp against a fake server, clock,
// filesystem and visitor, and reports how fast each pulled sheet reaches the server.
//   ./run.sh [fresh|steady|closed-idle|flaky|rebuild] [log]
#include "Arduino.h"
#include <map>
#include <set>
#include <vector>
#include <random>
#include "WiFi.h"
SerialT Serial; EspT ESP; WiFiClass WiFi;
size_t g_fake_internal_free = 62000, g_fake_internal_block = 31000; int g_fake_wifi_rejoins = 0;
#include "LittleFS.h"
FakeFS g_fs; LittleFSClass LittleFS;
size_t FakeFS::used() const { size_t u = 8 * 4096; for (auto& kv : files) u += ((kv.second.size() + 4095) / 4096 + 1) * 4096; return u; }

#include "../../holder/sync.cpp"   // the code under test, unchanged

// ---- clock, visitor and speech model ----------------------------------------
static uint64_t g_now = 0, g_end = 0;
uint32_t millis() { return (uint32_t)g_now; }
struct SimEnd {};
static HolderStatus st;
struct Pull { uint64_t start, end; double sheets_per_s; };
static std::vector<Pull> pulls;
static uint64_t speech_until = 0, next_sheet_at = 0;
static uint32_t true_sheets = 0;
static std::vector<std::pair<uint64_t, uint32_t>> changes;      // (time, value) of each true change
static int speech_after_pull_ms = 15000;
static bool quiet_log = true;
static void sim_step() {
  for (auto& p : pulls) {
    if (g_now >= p.start && g_now < p.end) {
      if (next_sheet_at < p.start) next_sheet_at = p.start + (uint64_t)(1000 / p.sheets_per_s);
      if (g_now >= next_sheet_at) {
        ++true_sheets; st.sheets_live = true_sheets; net::request_now();
        changes.push_back({g_now, true_sheets});
        next_sheet_at += (uint64_t)(1000 / p.sheets_per_s);
      }
      speech_until = std::max<uint64_t>(speech_until, g_now + speech_after_pull_ms);
    }
  }
  st.playing = g_now < speech_until;
}
static void advance(uint64_t ms) { while (ms) { uint64_t d = std::min<uint64_t>(ms, 10); g_now += d; ms -= d; sim_step(); } }
void vTaskDelay(uint32_t ms) { advance(ms); if (g_now >= g_end) throw SimEnd(); }
void delay(uint32_t ms) { advance(ms); }

// ---- fake server + link ------------------------------------------------------
static struct {
  std::string version = "news-100-abc";
  int items = 24; size_t clip = 107000, sys = 18000;
  int poll_s = 2;
  bool up = false; uint64_t last_activity = 0;
  uint32_t idle_close_ms = 60000, handshake_ms = 1500, rtt_ms = 150; double kb_per_ms = 0.3;
  int fail_connects = 0; double media_fail = 0; std::set<std::string> missing;
  std::vector<std::pair<uint64_t, int>> beats;
  int handshakes = 0, media_gets = 0, bulletin_gets = 0, beat_posts = 0;
} S;
static std::mt19937 rng(7);
bool fake_link_connected() {
  if (S.up && g_now - S.last_activity > S.idle_close_ms) S.up = false;   // server closed the idle connection
  return S.up;
}
void fake_link_stop() { S.up = false; }
FakeResponse fake_request(WiFiClientSecure&, const std::string& method, const std::string& url, const std::string& body) {
  if (!fake_link_connected()) {
    advance(S.handshake_ms); ++S.handshakes;
    if (S.fail_connects > 0) { --S.fail_connects; return {-1, "", false}; }
    S.up = true;
  }
  advance(S.rtt_ms); S.last_activity = g_now;
  std::string path = url.substr(strlen(SERVER_URL));
  if (method == "POST" && path == "/api/holder/" HOLDER_ID "/heartbeat") {
    ++S.beat_posts;
    JsonDocument d; deserializeJson(d, body.c_str());
    S.beats.push_back({g_now, d["sheets_this_roll"] | -1});
    if (!quiet_log) printf("%7.1fs beat sheets=%d items=%d fetching=%s\n", g_now / 1000.0, (int)(d["sheets_this_roll"] | -1), (int)(d["items"] | 0), (const char*)(d["fetching"] | ""));
    char out[200]; snprintf(out, sizeof out, "{\"source\":\"news\",\"bulletin\":\"%s\",\"page_open_ts\":0,\"roll_reset_ts\":0,\"poll_s\":%d}", S.version.c_str(), S.poll_s);
    return {200, out, true};
  }
  if (path == "/api/holder/" HOLDER_ID "/bulletin") {
    ++S.bulletin_gets;
    std::string j = "{\"version\":\"" + S.version + "\",\"source\":\"news\",\"source_label\":\"News\",\"items\":[";
    for (int i = 0; i < S.items; ++i) j += std::string(i ? "," : "") + "{\"id\":\"" + S.version + "-i" + std::to_string(i) + "\",\"text\":\"" + std::string(300, 'w') + "\",\"file\":\"" + S.version.substr(9) + "c" + std::to_string(i) + ".mp3\",\"duration_ms\":13000}";
    j += "],\"system\":{\"switch\":\"sw.mp3\",\"low_paper\":\"lo.mp3\",\"new_roll\":\"nr.mp3\",\"offline\":\"of.mp3\",\"empty\":\"em.mp3\"}}";
    return {200, j, true};
  }
  if (path.rfind("/media/", 0) == 0) {
    ++S.media_gets;
    std::string f = path.substr(7);
    if (S.missing.count(f)) return {404, "{\"detail\":\"Not Found\"}", true};
    bool sys = f.size() <= 6;
    return {200, std::string(sys ? S.sys : S.clip, 'x'), true};
  }
  return {404, "", true};
}
static bool s_cut = false;
int FakeBodyStream::available() {
  if (p == 0) { std::uniform_real_distribution<double> u(0, 1); s_cut = u(rng) < S.media_fail && b.size() > 50000; }
  if (s_cut && p > b.size() / 2) { S.up = false; return 0; }
  size_t n = std::min<size_t>(4096, b.size() - p);
  if (n) advance((uint64_t)(n / 1000.0 / S.kb_per_ms) + 1);
  return (int)n;
}
int FakeBodyStream::read(uint8_t* buf, size_t n) { n = std::min(n, b.size() - p); memcpy(buf, b.data() + p, n); p += n; return (int)n; }
uint8_t FakeBodyStream::connected() { return S.up; }
int fake_stream_body(const std::string& body, Stream* sink) {
  size_t done = 0;
  std::uniform_real_distribution<double> u(0, 1);
  bool fail = u(rng) < S.media_fail;
  while (done < body.size()) {
    size_t n = std::min<size_t>(4096, body.size() - done);
    advance((uint64_t)(n / 1000.0 / S.kb_per_ms) + 1);
    if (fail && done > body.size() / 2) { S.up = false; return HTTPC_ERROR_CONNECTION_LOST; }
    if (sink->write((const uint8_t*)body.data() + done, n) != n) return HTTPC_ERROR_STREAM_WRITE;
    done += n;
  }
  return (int)done;
}

// ---- run + report --------------------------------------------------------------
static int restarts = 0; static uint64_t first_restart = 0;
static void run(uint64_t until) {
  g_end = until;
  try { task(nullptr); } catch (SimEnd&) {} catch (RestartSignal&) { ++restarts; if (!first_restart) first_restart = g_now; }
}
static void report(const char* name, uint64_t from_ms = 0) {
  std::vector<double> lat;
  for (auto& c : changes) {
    if (c.first < from_ms) continue;
    double l = -1;
    for (auto& b : S.beats) if (b.first >= c.first && b.second >= (int)c.second) { l = (b.first - c.first) / 1000.0; break; }
    lat.push_back(l);
  }
  double worst = 0, sum = 0; int never = 0;
  for (double l : lat) { if (l < 0) ++never; else { worst = std::max(worst, l); sum += l; } }
  double gap = 0; for (size_t i = 1; i < S.beats.size(); ++i) if (S.beats[i].first >= from_ms) gap = std::max(gap, (S.beats[i].first - S.beats[i - 1].first) / 1000.0);
  size_t cached = 0; for (auto& kv : g_fs.files) if (kv.first.rfind("/m/", 0) == 0 && !String(kv.first.c_str()).endsWith(".part")) ++cached;
  printf("\n== %s\n   sheets pulled %zu: reached server in avg %.2f s, worst %.2f s, never %d\n", name, lat.size(), lat.empty() ? 0 : sum / std::max<size_t>(1, lat.size() - never), worst, never);
  printf("   longest gap between heartbeats %.1f s | beats %d, handshakes %d, bulletin GETs %d, media GETs %d\n", gap, S.beat_posts, S.handshakes, S.bulletin_gets, S.media_gets);
  if (restarts) printf("   self-restart at %.1f s (connect failures %u, WiFi rejoins %d)\n", first_restart / 1000.0, g_connect_failures, g_fake_wifi_rejoins);
  else printf("   no self-restart (connect failures %u, WiFi rejoins %d)\n", g_connect_failures, g_fake_wifi_rejoins);
  printf("   holder has %s, %u items handed over, %zu files cached, fs used %zu / %zu KB, fetch attempts %u, wanted %d\n",
         g_current_version, g_items_cached, cached, g_fs.used() / 1024, g_fs.capacity / 1024, g_fetch_attempts, g_fetch_wanted);
}
static void boot() { net::begin(&st); }

int main(int argc, char** argv) {
  std::string sc = argc > 1 ? argv[1] : "fresh";
  if (argc > 2) quiet_log = false;
  boot();
  if (sc == "fresh") {
    // Empty flash, page open, a visitor pulling three times while the whole feed downloads.
    pulls = {{5000, 12000, 1.0}, {30000, 36000, 1.5}, {60000, 64000, 1.0}};
    run(180000); report("fresh boot, empty storage, page open, pulls during the download");
  } else if (sc == "steady") {
    run(150000);                                   // let the first download finish
    S.beats.clear(); changes.clear(); S.handshakes = S.media_gets = S.bulletin_gets = S.beat_posts = 0;
    pulls = {{160000, 170000, 1.0}, {200000, 203000, 2.0}};
    run(260000); report("steady state, page open (2 s poll), talking while pulling", 150000);
  } else if (sc == "closed-idle") {
    S.poll_s = 20; S.idle_close_ms = 60000;        // page closed; Railway drops idle connections after 60 s
    run(150000);
    S.beats.clear(); changes.clear(); S.handshakes = S.media_gets = S.bulletin_gets = S.beat_posts = 0;
    pulls = {{160000, 170000, 1.0}, {230000, 234000, 1.0}};
    run(300000); report("page closed (20 s poll), Railway closes idle connections after 60 s", 150000);
  } else if (sc == "flaky") {
    S.media_fail = 0.25; S.missing = {"100-abcc3.mp3"}; S.fail_connects = 2;
    pulls = {{20000, 30000, 1.0}};
    run(900000); report("25% of downloads cut off, one clip missing (404), first two connects fail");
  } else if (sc == "fragmented") {
    // Running fine, then the connection drops and the heap is too broken up for a new TLS session.
    run(150000);
    S.beats.clear(); changes.clear(); S.handshakes = S.media_gets = S.bulletin_gets = S.beat_posts = 0;
    g_fake_internal_free = 46000; g_fake_internal_block = 12000;
    S.up = false; S.fail_connects = 1000;
    pulls = {{152000, 158000, 1.0}};
    run(400000); report("connection lost, internal heap too fragmented to reconnect (visitor pulling)", 150000);
  } else if (sc == "outage") {
    // Plenty of memory, but the internet is down: rejoin WiFi first, restart only as a last resort.
    run(150000);
    S.beats.clear(); changes.clear(); S.handshakes = S.media_gets = S.bulletin_gets = S.beat_posts = 0;
    S.up = false; S.fail_connects = 1000;
    run(900000); report("internet down for 12 minutes, memory fine", 150000);
  } else if (sc == "rebuild") {
    run(150000);
    S.beats.clear(); changes.clear(); S.handshakes = S.media_gets = S.bulletin_gets = S.beat_posts = 0;
    S.version = "news-200-def";                      // the server's half-hourly rebuild: all-new clips
    pulls = {{155000, 165000, 1.0}, {185000, 190000, 1.0}};
    run(400000); report("server rebuilds the feed (all new clips) while a visitor pulls", 150000);
  }
  return 0;
}
