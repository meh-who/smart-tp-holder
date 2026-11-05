#pragma once
#include "Arduino.h"
bool fake_link_connected();
void fake_link_stop();
class WiFiClientSecure {
 public:
  uint8_t connected() { return fake_link_connected(); }
  void stop() { fake_link_stop(); }
  void setInsecure() {}
  void setTimeout(int) {}
  void setHandshakeTimeout(int s) { handshake_timeout_s = s; }
  int lastError(char* b, size_t n) { strlcpy(b, "fake", n); return 0; }
  int handshake_timeout_s = 120;
};
