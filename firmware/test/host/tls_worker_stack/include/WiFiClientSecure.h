#pragma once
#include <cstdint>
class WiFiClient { public: virtual ~WiFiClient()=default; };
class WiFiClientSecure: public WiFiClient { public: void setCACert(const char*) {} void setTimeout(uint32_t) {} };
