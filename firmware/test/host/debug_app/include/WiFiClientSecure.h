#pragma once
#include "HTTPClient.h"
class WiFiClientSecure : public WiFiClient {
 public:
  ~WiFiClientSecure() override {
    if (test_platform::on_tls_destroy) test_platform::on_tls_destroy();
  }
  void setCACert(const char*) {}
  void setTimeout(uint32_t) {}
};
