// sync.h — the only WiFi code in the firmware.
//
// Runs as a FreeRTOS task on core 0 so that downloads never stall audio or
// encoder polling on core 1. It:
//   * keeps WiFi up,
//   * POSTs a heartbeat every POLL_*_S (the companion web page reads this to
//     show "online / last pull / cached items"),
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
  volatile uint32_t sheets_this_roll = 0;
  volatile uint32_t last_pull_ms = 0;      // millis() of the last PULL_STARTED
  volatile uint8_t  queued = 0;
  volatile bool     playing = false;
  volatile int32_t  rssi = 0;
  volatile bool     online = false;
  volatile uint32_t last_sync_ms = 0;
  volatile uint32_t last_sync_error = 0;   // HTTP code or -1
};

namespace sync {
void begin(HolderStatus* status);
bool take_bulletin(Bulletin& out, bool& source_changed);   // true if a new one is ready
bool online();
bool load_cached(Bulletin& out);                            // last bulletin from flash, at boot
void request_now();                                         // poke the task (e.g. after a button press)
void note_played(const char* item_id);                      // reported in the next heartbeat
}
