#pragma once
#include <cstddef>
#include <cstdint>
#include <provisioning/secret_store.h>
namespace sigap::provisioning {
enum class WifiState : uint8_t { Unconfigured, Connecting, Connected, Backoff, SetupRequired };
class IProvisioningClock { public: virtual ~IProvisioningClock() = default; virtual uint64_t now_us() const noexcept = 0; };
class ArduinoWifiStation {};
class WifiConnectionManager {
 public:
  WifiConnectionManager(NvsSecretStore&, ArduinoWifiStation&, IProvisioningClock&) noexcept {}
  bool begin() noexcept;
  void poll() noexcept;
  WifiState state() const noexcept { return WifiState::Unconfigured; }
};
class WifiProvisioningBackend { public: explicit WifiProvisioningBackend(WifiConnectionManager&) noexcept {} };
}
