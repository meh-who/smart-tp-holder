#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
#define WIFI_STA 1
struct IPAddressMock { String toString() const { return "10.0.0.2"; } };
extern int g_fake_wifi_rejoins;
struct WiFiClass { int status() { return WL_CONNECTED; } void mode(int) {} void setSleep(bool) {} void begin(const char*, const char*) {}
                   void disconnect() { ++g_fake_wifi_rejoins; }
                   int32_t RSSI() { return -55; } IPAddressMock localIP() { return {}; } };
extern WiFiClass WiFi;
