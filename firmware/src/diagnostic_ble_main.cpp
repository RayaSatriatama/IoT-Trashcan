#include <Arduino.h>
#include <esp_timer.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "provisioning/ble_provisioning.h"

extern "C" bool sigap_ble_diag_failed() noexcept;

namespace {
using namespace sigap::provisioning;

class DiagnosticBackend final : public IBleProvisioningBackend {
 public:
  // cppcheck-suppress unusedFunction
  WifiState wifi_state() const noexcept override { return WifiState::SetupRequired; }
  std::string_view current_ssid() const noexcept override { return {}; }
  WifiScanStatus request_scan() noexcept override { return WifiScanStatus::Failed; }
  WifiScanStatus poll_scan(WifiNetwork*, std::size_t,
                           std::size_t& count) noexcept override {
    count = 0U;
    return WifiScanStatus::Failed;
  }
  // cppcheck-suppress unusedFunction
  WifiScanStatus list_known_wifi(WifiNetwork*, std::size_t,
                                 std::size_t& count) noexcept override {
    count = 0U;
    return WifiScanStatus::Failed;
  }
  // cppcheck-suppress unusedFunction
  bool request_connect(std::string_view, std::string_view) noexcept override {
    return false;
  }
  // cppcheck-suppress unusedFunction
  bool retry_wifi() noexcept override { return false; }
  void cancel() noexcept override {}
};

class DiagnosticClock final : public BleProvisioningClock {
 public:
  // cppcheck-suppress unusedFunction
  uint64_t now_us() const noexcept override {
    const int64_t value = esp_timer_get_time();
    return value < 0 ? 0U : static_cast<uint64_t>(value);
  }
};

DiagnosticBackend backend{};
DiagnosticClock clock_source{};
BleProvisioningSession session{backend, clock_source};
ArduinoBleProvisioningService ble_service{};
bool halted{false};

void stop_with_reason(const char* reason) noexcept {
  if (halted) return;
  halted = true;
  Serial.print("BLE_DIAG_STOP reason=");
  Serial.println(reason);
}

}  // namespace

// cppcheck-suppress unusedFunction
void setup() {
  Serial.begin(115200);
  if (sigap_ble_diag_failed()) {
    stop_with_reason("nvs_error");
    return;
  }

  const uint64_t device_mac = ESP.getEfuseMac();
  session.set_device_id(device_mac);
  session.set_read_only_diagnostic(true);
  char device_name[32U]{};
  const int length = std::snprintf(
      device_name, sizeof(device_name), "SIGAP-%04llX",
      static_cast<unsigned long long>(device_mac & 0xFFFFU));
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(device_name)) {
    stop_with_reason("invalid_device_name");
    return;
  }
  if (!ble_service.begin(session, std::string_view(device_name))) {
    stop_with_reason(ble_service.diagnostic_failure_reason());
    return;
  }
  Serial.println("BLE_DIAG_READY wifi=DISABLED status=device_identity_bound");
}

// cppcheck-suppress unusedFunction
void loop() {
  if (sigap_ble_diag_failed()) {
    ble_service.poll();
    stop_with_reason("nvs_error");
    delay(50U);
    return;
  }
  if (!halted) {
    ble_service.poll();
  }
  delay(5U);
}
