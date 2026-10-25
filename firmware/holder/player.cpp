// player.cpp
#include "player.h"
#include "config.h"

void Player::begin() {
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW);
  out_ = new AudioOutputI2S();
  out_->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
  out_->SetOutputModeMono(true);
  out_->SetGain(powf(10.0f, GAIN_DB / 20.0f));
  mp3_ = new AudioGeneratorMP3();
}

void Player::amp(bool on) {
  digitalWrite(PIN_AMP_SD, on ? HIGH : LOW);
  if (!on) amp_off_at_ = 0;
}

uint32_t Player::backlog_ms() const {
  uint32_t ms = 0;
  if (playing()) {
    uint32_t elapsed = millis() - started_ms_;
    ms += (elapsed < current_ms_) ? current_ms_ - elapsed : 0;
  }
  for (uint8_t i = 0, idx = head_; i < count_; ++i, idx = (idx + 1) % kQ) ms += q_[idx].duration_ms;
  return ms;
}

bool Player::enqueue(const char* path, uint32_t duration_ms, const char* item_id) {
  if (count_ >= kQ) return false;
  if (backlog_ms() + duration_ms > BACKLOG_CAP_MS && count_ > 0) {
    Serial.printf("[player] backlog cap: dropping %s\n", path);
    return false;
  }
  QueuedClip& c = q_[tail_];
  strlcpy(c.path, path, sizeof c.path);
  strlcpy(c.item_id, item_id, sizeof c.item_id);
  c.duration_ms = duration_ms;
  tail_ = (tail_ + 1) % kQ; ++count_;
  return true;
}

void Player::play_now(const char* path, uint32_t duration_ms) {
  stop();
  QueuedClip c; strlcpy(c.path, path, sizeof c.path); c.item_id[0] = 0; c.duration_ms = duration_ms;
  start(c);
}

void Player::clear() { stop(); head_ = tail_ = count_ = 0; }

void Player::start(const QueuedClip& c) {
  if (!LittleFS.exists(c.path)) { Serial.printf("[player] missing %s\n", c.path); return; }
  amp(true);
  file_ = new AudioFileSourceLittleFS(c.path);
  if (!mp3_->begin(file_, out_)) {
    Serial.printf("[player] cannot start %s\n", c.path);
    delete file_; file_ = nullptr;
    return;
  }
  started_ms_ = millis(); current_ms_ = c.duration_ms;
  if (c.item_id[0]) strlcpy(last_item_id_, c.item_id, sizeof last_item_id_);
  strlcpy(last_path_, c.path, sizeof last_path_);
  last_duration_ = c.duration_ms;
  Serial.printf("[player] > %s (%lu ms)\n", c.path, (unsigned long)c.duration_ms);
}

void Player::stop() {
  if (mp3_ && mp3_->isRunning()) mp3_->stop();
  if (file_) { delete file_; file_ = nullptr; }
}

void Player::loop() {
  if (mp3_ && mp3_->isRunning()) {
    if (!mp3_->loop()) {          // clip finished
      stop();
      amp_off_at_ = millis() + 400;   // keep the amp up briefly between clips
    }
    return;
  }
  if (count_ > 0) {
    QueuedClip c = q_[head_];
    head_ = (head_ + 1) % kQ; --count_;
    start(c);
    return;
  }
  if (amp_off_at_ && millis() > amp_off_at_) amp(false);
}
