// sync.h — the only WiFi code in the firmware.
//
// Runs as a FreeRTOS task on core 0 so that downloads never stall audio or
// encoder polling on core 1. It:
//   * keeps WiFi up,
//   * POSTs a heartbeat every POLL_*_S (the companion web page reads this to
//     show "online / last pull / cached items"), and straight away whenever the
//     sheet count changes, at most once per MIN_BEAT_MS, so the page's roll
//     counts down as the paper is pulled. Heartbeats reuse one kept-alive TLS
//     connection, so they are cheap enough to send while the holder is speaking;
//     anything that needs a fresh TLS handshake or a download still waits for
//     the current clip to finish,
//   * learns from the heartbeat reply which source the visitor chose and
//     which bulletin version is current,
//   * fetches bulletin.json + any missing MP3s into LittleFS and swaps the
//     new bulletin in atomically.
#pragma once
#include <Arduino.h>

struct BulletinItem {
  char     id[24];
  char     file[40];     // path on LittleFS, e.g. /m/ab12cd34.mp3
  uint32_t duration_ms;
};

struct Bulletin {
  char     version[40] = "";
  char     source[16]  = "";      // news | x | tiktok | reels
  char     source_label[32] = "";
  uint8_t  count = 0;
  BulletinItem items[40];
  char     sys_switch[40]   = "";
  char     sys_low[40]      = "";
  char     sys_newroll[40]  = "";
  char     sys_offline[40]  = "";
  char     sys_empty[40]    = "";
};

// Telemetry the main loop keeps current for the heartbeat.
struct HolderStatus {
  volatile uint32_t sheets_total = 0;
  volatile uint32_t sheets_this_roll = 0;  // charged when a pull session ends (saved to flash)
  volatile uint32_t sheets_live = 0;       // sheets_this_roll + the pull in progress: what the page shows
  volatile uint32_t last_pull_ms = 0;     // millis() of the last PULL_STARTED
  volatile uint8_t  queued = 0;
  volatile bool     playing = false;
  volatile int32_t  rssi = 0;
  volatile bool     online = false;
  volatile uint32_t last_sync_ms = 0;
  volatile uint32_t last_sync_error = 0;   // HTTP code or -1
};

namespace net {
void begin(HolderStatus* status);
bool take_bulletin(Bulletin& out, bool& source_changed);   // true if a new one is ready
bool online();
bool load_cached(Bulletin& out);                            // last bulletin from flash, at boot
void request_now();                                         // heartbeat soon (a sheet pulled, a button press)
void note_played(const char* item_id);                      // reported in the next heartbeat
bool phone_tapped();                                        // true once per new companion-page session
void set_roll_reset_seen(double ts);                        // at boot, from flash: last "New roll" applied
bool take_roll_reset(double& ts);                           // true once when the page asked for a new roll
}
