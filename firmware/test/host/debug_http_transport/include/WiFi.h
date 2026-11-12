#pragma once
class WiFiClass { public: bool connected{true}; bool isConnected() const { return connected; } };
extern WiFiClass WiFi;
