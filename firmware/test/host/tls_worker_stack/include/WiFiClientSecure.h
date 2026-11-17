#pragma once
#include <cstdint>
class WiFiClient { public: virtual ~WiFiClient()=default; };
class WiFiClientSecure: public WiFiClient {
 public:
  void setCACert(const char*) {}
  int setTimeout(uint32_t) { return 0; }
  void setHandshakeTimeout(unsigned long) {}
};
