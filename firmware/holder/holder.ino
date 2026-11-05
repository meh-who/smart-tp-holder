// holder.ino — (S)wipable, Scrollable, Flushable: firmware for the toilet-roll
// holder. Adafruit ESP32-S2 Feather (also HUZZAH32 / ESP32-S3). Revision 2.1.
//
// What changed from Revision C:
//   * The holder is mains-powered in the gallery, so it stays online and asks
//     the server every 20 s which source the visitor chose on the companion
//     page (opened by tapping the NFC tag on the holder with a phone).
//   * It POSTs a heartbeat so the page can show "online / last pull / items
//     cached / paper left".
//   * A source change is announced with a short clip ("Now reading X.") and
//     the item queue is cleared, so the next sheet speaks from the new feed.
//   * One status LED: breathes while waiting, double-flashes when a phone opens
//     the companion page, steady when online with a feed (led.h).
//
// Libraries (Arduino Library Manager): ESP32Encoder, ESP8266Audio, ArduinoJson 7.
// Boards Manager: "esp32" by Espressif, 3.x.
//
// Adafruit ESP32-S2 Feather (single core, native USB-C, 4 MB flash, 2 MB PSRAM):
//   Board: "Adafruit Feather ESP32-S2"  (or "ESP32S2 Dev Module")
//   USB CDC On Boot: Enabled   (otherwise Serial Monitor shows nothing)
//   Partition Scheme: "No OTA (2MB APP/2MB SPIFFS)"
//   First upload only: hold BOOT, tap RESET, release BOOT, then Upload; tap RESET
//   when it finishes. After that, uploads work without the buttons.
//
// Adafruit HUZZAH32 (original ESP32, 4 MB flash, no PSRAM):
//   Board: "Adafruit ESP32 Feather"  (or "ESP32 Dev Module")
//   Partition Scheme: "No OTA (2MB APP/2MB SPIFFS)"   <- needed: the default leaves
//   too little room for the clips. The sketch formats that partition as LittleFS.
//   Upload Speed 921600; the board auto-resets into the bootloader over USB.
// ESP32-S3 boards: "ESP32S3 Dev Module", USB CDC On Boot: Enabled, a partition
//   scheme with >= 2 MB for the filesystem.
//
// Serial console (115200):  enc        toggle a live encoder readout (wiring check)
//                           cal <mm>   calibrate after pulling exactly <mm> of paper
//                           newroll    reset the roll counter
//                           sync       force a server sync now
//                           status     print state
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ESP32Encoder.h>
#include "config.h"
#include "bs_motion.h"
#include "player.h"
#include "sync.h"
#include "led.h"

static ESP32Encoder    encoder;
static bs::MotionEngine engine;
static Player          player;
static HolderStatus    status;
static Bulletin        bulletin;
static Preferences     prefs;
static StatusLed       led;

static float    mm_per_count = MM_PER_COUNT_DEFAULT;
static int64_t  last_count = 0;
static int64_t  cal_start_count = 0;
static uint32_t last_poll_ms = 0;
static uint8_t  cursor = 0;              // next item to read from the bulletin
static bool     low_paper_said = false;
static uint32_t button_down_ms = 0;
static bool     button_was_down = false;
static bool     enc_debug = false;      // console 'enc' toggles a live count readout

// ---- Played-item memory: nothing repeats within twelve hours -------------------
// The server already excludes items a holder reported as played; this is the
// local copy so a reboot does not replay the morning's items.
static const int kPlayedMax = 64;
static char     played[kPlayedMax][24];
static uint8_t  played_n = 0, played_head = 0;

static bool was_played(const char* id) {
  for (uint8_t i = 0; i < played_n; ++i) if (!strcmp(played[i], id)) return true;
  return false;
}
static void mark_played(const char* id) {
  if (!*id || was_played(id)) return;
  strlcpy(played[played_head], id, 24);
  played_head = (played_head + 1) % kPlayedMax;
  if (played_n < kPlayedMax) ++played_n;
  File f = LittleFS.open("/played.txt", played_n == 1 ? "w" : "a");
  if (f) { f.println(id); f.close(); }
}
static void load_played() {
  File f = LittleFS.open("/played.txt", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length()) { strlcpy(played[played_head], line.c_str(), 24); played_head = (played_head + 1) % kPlayedMax; if (played_n < kPlayedMax) ++played_n; }
  }
  f.close();
}

// ---- Items ---------------------------------------------------------------------
static bool enqueue_next_item() {
  if (bulletin.count == 0) {
    if (bulletin.sys_empty[0] && !player.playing()) player.play_now(bulletin.sys_empty, 3000);
    return false;
  }
  for (uint8_t tries = 0; tries < bulletin.count; ++tries) {
    BulletinItem& it = bulletin.items[cursor];
    cursor = (cursor + 1) % bulletin.count;
    if (was_played(it.id)) continue;
    if (player.enqueue(it.file, it.duration_ms, it.id)) { mark_played(it.id); net::note_played(it.id); return true; }
    return false;   // backlog cap: stop trying
  }
  // Everything cached has been heard: ask the server for more and say so once.
  net::request_now();
  if (bulletin.sys_empty[0] && !player.playing()) player.play_now(bulletin.sys_empty, 3000);
  return false;
}

static void adopt_bulletin(const Bulletin& b, bool announce_switch) {
  bulletin = b;
  cursor = 0;
  if (announce_switch) {
    player.clear();
    if (bulletin.sys_switch[0]) player.play_now(bulletin.sys_switch, 2500);
    Serial.printf("[holder] source -> %s\n", bulletin.source);
  }
}

// ---- New roll: the button, the serial console and the companion page all land here.
static void new_roll() {
  status.sheets_this_roll = 0;
  low_paper_said = false;
  prefs.putUInt("roll", 0);
  if (bulletin.sys_newroll[0]) player.play_now(bulletin.sys_newroll, 2000);
  Serial.println("[holder] new roll: count reset to 0");
  net::request_now();                // tell the page promptly
}

// ---- Console -------------------------------------------------------------------
static void handle_serial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (line.startsWith("cal")) {
    float mm = line.substring(3).toFloat();
    int64_t counts = encoder.getCount() - cal_start_count;
    if (mm > 0 && counts != 0) {
      mm_per_count = fabsf(mm / (float)counts);
      prefs.putFloat("mmpc", mm_per_count);
      Serial.printf("[cal] %lld counts for %.0f mm -> %.4f mm/count (saved)\n", (long long)counts, mm, mm_per_count);
    } else Serial.println("[cal] usage: type 'cal' marks the start; pull paper; 'cal 570' finishes");
    cal_start_count = encoder.getCount();
  } else if (line == "newroll") {
    new_roll();
  } else if (line == "enc") {
    enc_debug = !enc_debug;
    Serial.printf("[enc] live readout %s. Pins now A(%d)=%d B(%d)=%d, count %lld\n", enc_debug ? "ON" : "off",
                  PIN_ENC_A, digitalRead(PIN_ENC_A), PIN_ENC_B, digitalRead(PIN_ENC_B), (long long)encoder.getCount());
  } else if (line == "sync") {
    net::request_now();
  } else if (line == "status") {
    Serial.printf("fw %s  online %d  source %s  bulletin %s  items %u  cursor %u\n", FW_VERSION, status.online,
                  bulletin.source, bulletin.version, bulletin.count, cursor);
    Serial.printf("sheets total %lu  this roll %lu  mm/count %.4f  queue %u  playing %d  fs free %lu kB\n",
                  (unsigned long)status.sheets_total, (unsigned long)status.sheets_this_roll, mm_per_count,
                  player.queued(), player.playing(), (unsigned long)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
  }
}

// ---- Button: short press repeats the last item, hold 2 s = new roll ------------
static void handle_button(uint32_t now) {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  if (down && !button_was_down) button_down_ms = now;
  if (!down && button_was_down) {
    uint32_t held = now - button_down_ms;
    if (held >= 2000) {
      new_roll();
    } else if (held > 30 && player.last_path()[0]) {
      player.play_now(player.last_path(), player.last_duration());
    }
  }
  button_was_down = down;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[holder] %s fw %s\n", HOLDER_ID, FW_VERSION);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  if (!LittleFS.begin(true)) Serial.println("[holder] LittleFS failed");

  prefs.begin("holder", false);
  mm_per_count = prefs.getFloat("mmpc", MM_PER_COUNT_DEFAULT);
  status.sheets_total = prefs.getUInt("total", 0);
  status.sheets_this_roll = prefs.getUInt("roll", 0);

  // Quadrature on the PCNT peripheral, x4, with the hardware glitch filter as
  // a second layer behind the 10 k / 10 nF RC debounce on the board.
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachFullQuad(PIN_ENC_A, PIN_ENC_B);
  encoder.setFilter(1000);   // 12.5 µs glitch filter (1023 is just over the S2's hardware limit)
  encoder.clearCount();
  last_count = 0;

  player.begin();
  led.begin(PIN_LED);
  led.set(StatusLed::Mode::WAITING);
  load_played();
  if (net::load_cached(bulletin)) Serial.printf("[holder] cached bulletin %s (%u items, %s)\n", bulletin.version, bulletin.count, bulletin.source);
  net::set_roll_reset_seen(prefs.getDouble("rrts", 0));
  net::begin(&status);
  last_poll_ms = millis();
}

void loop() {
  uint32_t now = millis();
  player.loop();

  // 10 ms encoder poll -> gesture engine.
  if (now - last_poll_ms >= 10) {
    last_poll_ms = now;
    int64_t c = encoder.getCount();
    int64_t d = c - last_count; last_count = c;
#if ENCODER_INVERT
    d = -d;
#endif
    engine.feed((float)d * mm_per_count, now);
    if (enc_debug && d) {
      static uint32_t last_print = 0;
      if (now - last_print > 80) { Serial.printf("[enc] %+lld counts (%.1f mm)  total %lld\n", (long long)d, d * mm_per_count, (long long)c); last_print = now; }
    }

    bs::Event e;
    while (engine.next_event(e)) {
      switch (e.type) {
        case bs::EventType::PULL_STARTED:
          player.amp(true);                 // 33 mm of warm-up before the first word
          status.last_pull_ms = now;
          break;
        case bs::EventType::ITEM:
          enqueue_next_item();
          break;
        case bs::EventType::PULL_ENDED:
          break;
        case bs::EventType::SESSION_ENDED: {
          // Charge the roll for what was pulled (whole sheets, rounded).
          uint32_t sheets = (uint32_t)((e.travel_mm + engine.config().sheet_mm / 2) / engine.config().sheet_mm);
          status.sheets_this_roll += sheets;
          status.sheets_total += sheets;
          prefs.putUInt("roll", status.sheets_this_roll);
          prefs.putUInt("total", status.sheets_total);
          if (!low_paper_said && ROLL_SHEETS - (int)status.sheets_this_roll <= LOW_PAPER_SHEETS && bulletin.sys_low[0]) {
            low_paper_said = true;
            player.enqueue(bulletin.sys_low, 2500);
          }
          break;
        }
        default: break;
      }
    }
  }

  // A new bulletin from the sync task?
  { Bulletin b; bool switched = false;
    if (net::take_bulletin(b, switched)) adopt_bulletin(b, switched); }

  status.queued = player.queued();
  status.playing = player.playing();

  // LED: steady once we are online with a feed to read; breathe while waiting;
  // a double flash the moment a phone opens the companion page.
  led.set((status.online && bulletin.count > 0) ? StatusLed::Mode::READY : StatusLed::Mode::WAITING);
  if (net::phone_tapped()) led.paired();
  { double ts; if (net::take_roll_reset(ts)) { prefs.putDouble("rrts", ts); new_roll(); } }
  led.loop();

  handle_button(now);
  handle_serial();

#if !MAINS_POWERED
  // Battery: light-sleep between pulls once everything is quiet. GPIO wake on
  // either encoder line; RAM, I2S and the filesystem all survive.
  static uint32_t quiet_since = 0;
  bool busy = player.playing() || player.queued() || engine.in_session() || (now - status.last_sync_ms < 2000);
  if (busy) quiet_since = now;
  else if (now - quiet_since > 20000) {
    gpio_wakeup_enable((gpio_num_t)PIN_ENC_A, digitalRead(PIN_ENC_A) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_ENC_B, digitalRead(PIN_ENC_B) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)POLL_BATTERY_S * 1000000ULL);
    esp_light_sleep_start();
    quiet_since = millis();
  }
#endif
  delay(1);
}
