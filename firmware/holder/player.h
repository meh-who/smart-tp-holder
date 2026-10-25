// player.h — item queue and I2S MP3 playback through a MAX98357A.
//
// Uses ESP8266Audio (earlephilhower), which runs on the ESP32-S3:
//   Library Manager -> "ESP8266Audio".
// The queue holds file paths on LittleFS; the backlog cap counts speech
// milliseconds, not items, so a four-sheet pull of long headlines is trimmed
// to about twenty seconds of talking after you leave the room — deliberately
// loose, see the paper's section 6.
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "AudioFileSourceLittleFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

struct QueuedClip {
  char     path[48];
  uint32_t duration_ms;
  char     item_id[24];   // empty for system clips
};

class Player {
 public:
  void begin();
  void loop();                                    // call every loop() iteration
  bool enqueue(const char* path, uint32_t duration_ms, const char* item_id = "");
  void play_now(const char* path, uint32_t duration_ms);   // system clip, jumps the queue
  void clear();
  void amp(bool on);

  bool     playing() const { return mp3_ && mp3_->isRunning(); }
  uint8_t  queued() const  { return count_; }
  uint32_t backlog_ms() const;
  const char* last_item_id() const { return last_item_id_; }
  const char* last_path() const { return last_path_; }
  uint32_t last_duration() const { return last_duration_; }

 private:
  void start(const QueuedClip& c);
  void stop();

  static constexpr int kQ = 8;
  QueuedClip q_[kQ];
  uint8_t head_ = 0, tail_ = 0, count_ = 0;

  AudioFileSourceLittleFS* file_ = nullptr;
  AudioGeneratorMP3*       mp3_  = nullptr;
  AudioOutputI2S*          out_  = nullptr;
  uint32_t                 started_ms_ = 0, current_ms_ = 0;
  char                     last_item_id_[24] = "";
  char                     last_path_[48] = "";
  uint32_t                 last_duration_ = 0;
  uint32_t                 amp_off_at_ = 0;
};
