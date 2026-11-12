#pragma once
#include "HTTPClient.h"
class WiFiClientSecure : public WiFiClient { public: ~WiFiClientSecure() override; void setCACert(const char*); };
