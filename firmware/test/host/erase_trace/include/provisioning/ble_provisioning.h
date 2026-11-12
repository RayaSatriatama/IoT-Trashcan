#pragma once
#include <string_view>
#include <provisioning/wifi_manager.h>
namespace sigap::provisioning {
class BleProvisioningClock { public: virtual ~BleProvisioningClock() = default; virtual uint64_t now_us() const noexcept = 0; };
class BleProvisioningSession {
 public:
  BleProvisioningSession(WifiProvisioningBackend&, BleProvisioningClock&) noexcept {}
  void set_device_id(uint64_t) noexcept {}
};
class ArduinoBleProvisioningService {
 public:
  bool begin(BleProvisioningSession&, std::string_view) noexcept;
  void poll() noexcept;
};
}
