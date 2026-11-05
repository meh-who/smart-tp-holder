#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#define HTTP_CODE_OK 200
#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_CONNECTION_LOST (-5)
#define HTTPC_ERROR_TOO_LESS_RAM (-8)
#define HTTPC_ERROR_ENCODING (-9)
#define HTTPC_ERROR_STREAM_WRITE (-10)
#define HTTPC_ERROR_READ_TIMEOUT (-11)
struct FakeResponse { int code; std::string body; bool keep_alive; };
FakeResponse fake_request(WiFiClientSecure& c, const std::string& method, const std::string& url, const std::string& body);
int fake_stream_body(const std::string& body, Stream* sink);
// The body as the TLS stream delivers it: available()/read() advance the fake
// clock and can be cut off or stall (see fake_body_* in sync_sim.cpp).
struct FakeBodyStream {
  std::string b; size_t p = 0; WiFiClientSecure* c = nullptr;
  int available();
  int read(uint8_t* buf, size_t n);
  uint8_t connected();
};
class HTTPClient {
 public:
  ~HTTPClient() { if (c_) c_->stop(); }   // the real destructor stops the client too
  bool begin(WiFiClientSecure& c, const String& url) { c_ = &c; url_ = url.str(); resp_ = {}; return true; }
  void addHeader(const char*, const char*) {}
  int GET() { resp_ = fake_request(*c_, "GET", url_, ""); return resp_.code; }
  int POST(const String& b) { resp_ = fake_request(*c_, "POST", url_, b.str()); return resp_.code; }
  struct BodyStream : Stream { std::string b; size_t p = 0; int available() override { return (int)(b.size() - p); }
    int read() override { return p < b.size() ? (uint8_t)b[p++] : -1; } size_t write(uint8_t) override { return 0; } } body_;
  Stream& getStream() { body_.b = resp_.body; body_.p = 0; return body_; }
  int getSize() { return (int)resp_.body.size(); }
  String getString() { return String(resp_.body); }
  int writeToStream(Stream* s) { int n = fake_stream_body(resp_.body, s); if (n < 0) c_->stop(); return n; }
  void end() { if (!resp_.keep_alive && c_) c_->stop(); }
  void setReuse(bool) {}
  void setConnectTimeout(int32_t) {}
  void setTimeout(uint16_t) {}
  FakeBodyStream* getStreamPtr() { body2_.b = resp_.body; body2_.p = 0; body2_.c = c_; return &body2_; }
  static String errorToString(int e) { return String(e == -1 ? "connection refused" : e == -5 ? "connection lost" : e == -10 ? "stream write" : e == -11 ? "read timeout" : "error"); }
 private:
  WiFiClientSecure* c_ = nullptr; std::string url_; FakeResponse resp_{}; FakeBodyStream body2_;
};
