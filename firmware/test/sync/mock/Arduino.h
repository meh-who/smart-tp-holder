// Minimal host stand-ins for the Arduino/ESP32 APIs sync.cpp uses.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <algorithm>
#define CONFIG_IDF_TARGET_ESP32S2 1
uint32_t millis();
void delay(uint32_t ms);

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* b, size_t n) { size_t k = 0; while (n--) { if (!write(*b++)) break; ++k; } return k; }
};
class Printable { public: virtual ~Printable() {} virtual size_t printTo(Print& p) const = 0; };
class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() { return -1; }
  size_t readBytes(char* buf, size_t n) { size_t k = 0; while (k < n) { int c = read(); if (c < 0) break; buf[k++] = (char)c; } return k; }
  void setTimeout(unsigned long) {}
};
class String {
  std::string s_;
 public:
  String() {}
  String(const char* c) : s_(c ? c : "") {}
  String(const std::string& x) : s_(x) {}
  const char* c_str() const { return s_.c_str(); }
  unsigned length() const { return (unsigned)s_.size(); }
  bool concat(const char* c) { s_ += c; return true; }
  bool concat(const char* c, unsigned n) { s_.append(c, n); return true; }
  bool concat(char c) { s_ += c; return true; }
  bool reserve(unsigned n) { s_.reserve(n); return true; }
  String& operator+=(const String& o) { s_ += o.s_; return *this; }
  String& operator+=(const char* o) { s_ += o; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
  friend String operator+(const String& a, const char* b) { return String(a.s_ + b); }
  bool operator==(const char* o) const { return s_ == (o ? o : ""); }
  bool operator==(const String& o) const { return s_ == o.s_; }
  bool endsWith(const char* e) const { size_t n = strlen(e); return s_.size() >= n && s_.compare(s_.size() - n, n, e) == 0; }
  int indexOf(const char* x) const { auto p = s_.find(x); return p == std::string::npos ? -1 : (int)p; }
  const std::string& str() const { return s_; }
};
struct SerialT {
  void printf(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
  void println(const char* s) { puts(s); }
};
extern SerialT Serial;
struct RestartSignal {};
struct EspT { uint32_t getFreeHeap() { return 120000; } uint32_t getMinFreeHeap() { return 90000; } [[noreturn]] void restart() { throw RestartSignal(); } };
extern EspT ESP;

// FreeRTOS
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
void vTaskDelay(uint32_t ms);
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, int, TaskHandle_t*, int) { return 1; }
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 9000; }
