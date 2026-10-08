#pragma once

#include <cstddef>
#include <cstdint>

namespace sigap::provisioning {

// BLE provisioning is an engineering policy.  It is compile-time owned and
// deliberately separate from AppConfig sampling/storage policy.
struct ProvisioningPolicy final {
  uint32_t scan_cooldown_ms{15000U};
  uint32_t wifi_connect_timeout_ms{12000U};
  uint32_t session_timeout_ms{120000U};
  uint8_t max_scan_results{16U};
  uint16_t max_frame_bytes{192U};
  bool ble_service_enabled{true};

  constexpr bool valid() const noexcept {
    return scan_cooldown_ms > 0U && wifi_connect_timeout_ms > 0U &&
           session_timeout_ms > 0U && max_scan_results > 0U &&
           max_frame_bytes > 0U && max_frame_bytes <= 512U &&
           ble_service_enabled;
  }
};

inline constexpr ProvisioningPolicy kDefaultProvisioningPolicy{};

}  // namespace sigap::provisioning
