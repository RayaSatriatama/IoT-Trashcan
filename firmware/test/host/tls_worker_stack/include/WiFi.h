#pragma once
#define WIFI_STA 1
#define WL_CONNECTED 3
class WiFiClass { public: bool connected{true}; bool isConnected() const { return connected; } void mode(int) {} void begin(const char*,const char*) {} int status() const { return connected ? WL_CONNECTED : 0; } };
extern WiFiClass WiFi;
