// led.h — the one status LED, as described in the paper:
//
//   unlit          holder is off (nothing to do here)
//   slow pulse     powered, waiting for a connection (no server yet, or no feed)
//   double flash   a phone's companion session has just been associated
//   steady         connected and the feed is ready
//
// Non-blocking: call loop() every pass; it uses the LEDC PWM peripheral so a
// pulse costs nothing on the pull path.
#pragma once
#include <Arduino.h>

class StatusLed {
 public:
  enum class Mode : uint8_t { OFF, WAITING, READY };

  void begin(uint8_t pin);
  void set(Mode m);                // the resting state
  void paired();                   // play the double flash, then return to the resting state
  void loop();

 private:
  void write(uint8_t level);       // 0..255
  uint8_t  pin_ = 0;
  Mode     mode_ = Mode::OFF;
  uint32_t flash_until_ = 0;       // 0 = not flashing
};
