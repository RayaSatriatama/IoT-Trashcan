#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>

#include <cstdio>
#include <string_view>

#include "diagnostic_wifi_test_erase_trace.h"
#include "provisioning/ble_provisioning.h"
#include "provisioning/secret_store.h"
#include "provisioning/wifi_manager.h"

namespace {
using namespace sigap::provisioning;

class CandidateClock final : public IProvisioningClock,
                             public BleProvisioningClock {
 public:
  uint64_t now_us() const noexcept override {
    const int64_t value = esp_timer_get_time();
    return value < 0 ? 0U : static_cast<uint64_t>(value);
  }
};

NvsSecretStore secrets{};
ArduinoWifiStation station{};
CandidateClock clock_source{};
WifiConnectionManager wifi_manager{secrets, station, clock_source};
WifiProvisioningBackend backend{wifi_manager};
BleProvisioningSession session{backend, clock_source};
ArduinoBleProvisioningService ble_service{};
bool halted{false};
bool reported_ip{false};
bool erase_trace_reported{false};
bool ble_service_initialized{false};
bool ble_failure_cleanup_done{false};
const char* pending_stop_reason{nullptr};

const char* primitive_name(SigapErasePrimitive primitive) noexcept {
  switch (primitive) {
    case SigapErasePrimitive::nvs_flash_erase: return "nvs_flash_erase";
    case SigapErasePrimitive::nvs_flash_erase_partition: return "nvs_flash_erase_partition";
    case SigapErasePrimitive::nvs_flash_erase_partition_ptr: return "nvs_flash_erase_partition_ptr";
    case SigapErasePrimitive::nvs_erase_key: return "nvs_erase_key";
    case SigapErasePrimitive::nvs_erase_all: return "nvs_erase_all";
    case SigapErasePrimitive::esp_partition_erase_range: return "esp_partition_erase_range";
    case SigapErasePrimitive::none: return nullptr;
  }
  return nullptr;
}

const char* phase_name(SigapErasePhase phase) noexcept {
  switch (phase) {
    case SigapErasePhase::framework_startup: return "framework_startup";
    case SigapErasePhase::manager_begin: return "manager_begin";
    case SigapErasePhase::ble_begin: return "ble_begin";
    case SigapErasePhase::loop: return "loop";
  }
  return nullptr;
}

bool emit_erase_trace() noexcept {
  if (erase_trace_reported) return true;
  const SigapEraseDenial denial = sigap_ble_wifi_first_denial();
  if (!denial.ready || denial.caller_pc == 0) return false;
  const char* primitive = primitive_name(denial.primitive);
  const char* phase = phase_name(denial.phase);
  if (primitive == nullptr || phase == nullptr) return false;
  erase_trace_reported = true;
  Serial.printf("BLE_WIFI_TEST_ERASE primitive=%s phase=%s pc=0x%08lx\n",
                primitive, phase, static_cast<unsigned long>(denial.caller_pc));
  return true;
}

void report_pending_stop() noexcept {
  if (pending_stop_reason == nullptr) return;
  if (sigap_ble_wifi_erase_attempted() && !emit_erase_trace()) return;
  Serial.print("BLE_WIFI_TEST_STOP reason=");
  Serial.println(pending_stop_reason);
  pending_stop_reason = nullptr;
}

void stop_with_reason(const char* reason) noexcept {
  if (halted) return;
  halted = true;
  pending_stop_reason = reason;
  report_pending_stop();
}

void cleanup_ble_failure_once() noexcept {
  if (!ble_service_initialized || ble_failure_cleanup_done) return;
  ble_failure_cleanup_done = true;
  ble_service.poll();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  if (sigap_ble_wifi_erase_attempted()) {
    stop_with_reason("destructive_erase_blocked");
    return;
  }
  WiFi.persistent(false);

  const uint64_t device_id = ESP.getEfuseMac();
  session.set_device_id(device_id);
  char device_name[32]{};
  const int length = std::snprintf(
      device_name, sizeof(device_name), "SIGAP-%04llX",
      static_cast<unsigned long long>(device_id & 0xFFFFU));
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(device_name)) {
    stop_with_reason("invalid_device_name");
    return;
  }

  sigap_ble_wifi_set_phase(SigapErasePhase::manager_begin);
  if (!wifi_manager.begin()) {
    stop_with_reason("wifi_manager_start_failed");
    return;
  }
  if (sigap_ble_wifi_erase_attempted()) {
    stop_with_reason("destructive_erase_blocked");
    return;
  }

  sigap_ble_wifi_set_phase(SigapErasePhase::ble_begin);
  if (!ble_service.begin(session, std::string_view(device_name))) {
    stop_with_reason("ble_start_failed");
    return;
  }
  ble_service_initialized = true;
  if (sigap_ble_wifi_erase_attempted()) {
    cleanup_ble_failure_once();
    stop_with_reason("destructive_erase_blocked");
    return;
  }
  Serial.println("BLE_WIFI_TEST_READY wifi=production_provisioning");
}

void loop() {
  sigap_ble_wifi_set_phase(SigapErasePhase::loop);
  if (halted) {
    report_pending_stop();
    delay(5U);
    return;
  }
  if (sigap_ble_wifi_erase_attempted()) {
    cleanup_ble_failure_once();
    stop_with_reason("destructive_erase_blocked");
    return;
  }
  wifi_manager.poll();
  if (sigap_ble_wifi_erase_attempted()) {
    cleanup_ble_failure_once();
    stop_with_reason("destructive_erase_blocked");
    return;
  }
  if (wifi_manager.state() == WifiState::Connected && !reported_ip) {
    const IPAddress ip = WiFi.localIP();
    Serial.printf("BLE_WIFI_TEST_IP %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    reported_ip = true;
  } else if (wifi_manager.state() != WifiState::Connected) {
    reported_ip = false;
  }
  ble_service.poll();
  if (sigap_ble_wifi_erase_attempted()) {
    cleanup_ble_failure_once();
    stop_with_reason("destructive_erase_blocked");
    return;
  }
  delay(5U);
}
