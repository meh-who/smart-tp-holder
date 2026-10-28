// led.cpp — see led.h. Uses the ESP32 Arduino core 3.x LEDC API.
#include "led.h"

static const uint32_t kPulsePeriodMs = 2400;   // slow breath while waiting
static const uint32_t kFlashMs       = 90;     // on-time of each flash
static const uint32_t kFlashGapMs    = 110;    // gap between the two flashes
static const uint8_t  kSteady        = 160;    // steady brightness (0..255); 255 is harsh in a dark room

void StatusLed::begin(uint8_t pin) {
  pin_ = pin;
  ledcAttach(pin_, 5000, 8);     // 5 kHz, 8-bit
  write(0);
}

void StatusLed::write(uint8_t level) { ledcWrite(pin_, level); }

void StatusLed::set(Mode m) { mode_ = m; }

void StatusLed::paired() { flash_until_ = millis() + 2 * kFlashMs + kFlashGapMs; }

void StatusLed::loop() {
  uint32_t now = millis();

  if (flash_until_) {
    if (now >= flash_until_) { flash_until_ = 0; }
    else {
      uint32_t t = flash_until_ - now;                      // time remaining
      uint32_t total = 2 * kFlashMs + kFlashGapMs;
      uint32_t e = total - t;                               // elapsed
      bool on = (e < kFlashMs) || (e >= kFlashMs + kFlashGapMs);
      write(on ? 255 : 0);
      return;
    }
  }

  switch (mode_) {
    case Mode::OFF:    write(0); break;
    case Mode::READY:  write(kSteady); break;
    case Mode::WAITING: {
      // Cosine breath between ~4 and ~120, perceptually gentle.
      float phase = (now % kPulsePeriodMs) / (float)kPulsePeriodMs;
      float v = 0.5f - 0.5f * cosf(phase * 6.2831853f);
      write((uint8_t)(4 + v * v * 116));
      break;
    }
  }
}
