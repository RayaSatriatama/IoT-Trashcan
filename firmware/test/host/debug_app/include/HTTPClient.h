#pragma once
#include "Arduino.h"

class Stream {
 public:
  virtual ~Stream() = default;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual void flush() = 0;
  virtual size_t write(uint8_t) = 0;
  size_t write(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n && write(p[i])) ++i;
    return i;
  }
  virtual size_t readBytes(uint8_t* p, size_t n) {
    size_t i = 0;
    int b;
    while (i < n && (b = read()) >= 0) p[i++] = b;
    return i;
  }
  size_t readBytes(char* p, size_t n) {
    return readBytes(reinterpret_cast<uint8_t*>(p), n);
  }
};
class WiFiClient : public Stream {
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t) override { return 1; }
  bool connected() { return false; }
};
class HTTPClient {
 public:
  bool begin(WiFiClient&, String url) { url_ = url.c_str(); return true; }
  void setTimeout(uint16_t) {}
  void setConnectTimeout(int32_t) {}
  void addHeader(const String&, const String&, bool = false, bool = true) {}
  void collectHeaders(const char**, size_t) {}
  String header(const char*) { return String(""); }
  int GET() { ++test_platform::http_calls; body_.clear(); return 204; }
  int POST(uint8_t* p, size_t n) {
    ++test_platform::http_calls;
    body_.clear();
    if (url_.find("device-debug") != std::string::npos) {
      test_platform::last_debug_body.assign(reinterpret_cast<char*>(p), n);
      test_platform::last_debug_url = url_;
      test_platform::borrowed_body = reinterpret_cast<char*>(p);
      test_platform::borrowed_size = n;
    }
    return 503;
  }
  int getSize() { return body_.size(); }
  WiFiClient* getStreamPtr() { return &stream_; }
  int writeToStream(Stream* sink) {
    return sink->write(reinterpret_cast<const uint8_t*>(body_.data()), body_.size());
  }
  void end() { if (test_platform::on_http_end) test_platform::on_http_end(); }
 private:
  std::string url_, body_;
  WiFiClient stream_;
};
