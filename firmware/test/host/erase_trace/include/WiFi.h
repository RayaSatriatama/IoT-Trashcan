#pragma once
class IPAddress {
 public: unsigned char operator[](int index) const noexcept { return static_cast<unsigned char>(index); }
};
class FakeWiFi {
 public:
  void persistent(bool) noexcept {}
  IPAddress localIP() const noexcept { return {}; }
};
extern FakeWiFi WiFi;
