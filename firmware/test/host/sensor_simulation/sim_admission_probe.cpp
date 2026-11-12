// Actual app adapters/controller; synthetic SDK/NVS only, never hardware or TLS.
#include "sigap_app.h"
#include "fixtures.h"
#include "network/device_control_codec.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace sigap::board { void log_runtime_board_memory() noexcept {} }
namespace sigap {
struct DebugAppProbe {
  static maintenance::MaintenanceService& usb(SigapApp& app) { return app.maintenance_service_; }
  static maintenance::CommandRouterBleCalibrationBackend& ble(SigapApp& app) { return app.ble_calibration_backend_; }
  static ApplicationCalibrationExecutor& http(SigapApp& app) { return app.device_control_executor_; }
  static calibration::CalibrationTransportController& controller(SigapApp& app) { return app.calibration_controller_; }
  static config::ConfigManager& config(SigapApp& app) { return app.config_manager_; }
  static uint64_t generation(SigapApp& app) { return app.profile_manager_.generation(); }
  static bool workflow(SigapApp& app, calibration::CalibrationStatusSnapshot& output) {
    return app.maintenance_router_.status(output);
  }
  static std::size_t audit(SigapApp& app) { return app.audit_ring_.size(); }
  static bool profile(SigapApp& app, acquisition::EncodedProfile& output) {
    return app.profiles_.ds18b20 != nullptr &&
        acquisition::ProfileCodec{}.encode(*app.profiles_.ds18b20, output);
  }
};
}
namespace {
using namespace sigap;
int fail(const char* message) { std::fprintf(stderr, "FAIL %s\n", message); return 1; }
std::string digest(std::string_view bytes) {
  const auto hash = telemetry_contract::sha256(bytes);
  std::string result("sha256:");
  constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : hash) { result += hex[byte >> 4U]; result += hex[byte & 15U]; }
  return result;
}
bool command(std::string_view operation, std::string_view id, std::string_view payload,
             network::DeviceControlCommand& output) {
  // Exact field ordering/identity from internal/devicecontrol/store.go::commandRequestHash.
  const std::string identity = "\"device_id\":\"1234abcdef56\",\"operator_id\":\"host-operator\","
      "\"session_id\":\"host-session\",\"sensor_id\":\"hx711_organic\",\"operation\":\"" +
      std::string(operation) + "\",\"payload\":" + std::string(payload) +
      ",\"expires_at\":\"2026-10-29T23:59:59Z\"";
  const auto hash = digest("{" + identity + "}");
  const std::string wire = "{\"command_id\":\"" + std::string(id) + "\"," + identity +
      ",\"expires_in_ms\":100000,\"attempt\":1,\"request_hash\":\"" + hash + "\"}";
  return network::decode_device_control_command(wire, 1000U, output) &&
      output.request_hash.view() == hash && output.payload_size == payload.size();
}
struct State {
  config::EncodedConfig active{}, draft{};
  acquisition::EncodedProfile profile{};
  calibration::CalibrationStatusSnapshot workflow{};
  decltype(test_platform::nvs) durable{};
  uint64_t calls{}, writes{}, generation{};
  std::size_t audit{};
  bool owner{}, has_draft{}, has_profile{};
  bool capture(SigapApp& app) {
    const auto& config = DebugAppProbe::config(app);
    if (!config::ConfigCodec{}.encode(config.active(), active) ||
        !config::ConfigCodec{}.encode(config.draft(), draft)) return false;
    has_draft = config.has_draft();
    has_profile = DebugAppProbe::profile(app, profile);
    if (!DebugAppProbe::workflow(app, workflow)) return false;
    generation = DebugAppProbe::generation(app); audit = DebugAppProbe::audit(app);
    owner = DebugAppProbe::controller(app).active();
    durable = test_platform::nvs; calls = test_platform::nvs_calls; writes = test_platform::nvs_writes;
    return !durable.empty() && has_draft;
  }
  bool unchanged(SigapApp& app) const {
    State after{};
    if (!after.capture(app)) return false;
    if (calls != after.calls) std::printf("STATE nvs_calls_before=%llu after=%llu\n",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(after.calls));
    return active.bytes == after.active.bytes && active.hash() == after.active.hash() &&
        draft.bytes == after.draft.bytes && draft.hash() == after.draft.hash() &&
        has_draft == after.has_draft && has_profile == after.has_profile &&
        (!has_profile || (profile.bytes == after.profile.bytes && profile.hash() == after.profile.hash())) &&
        generation == after.generation && audit == after.audit && owner == after.owner &&
        workflow.active == after.workflow.active && workflow.state == after.workflow.state &&
        workflow.target == after.workflow.target && workflow.failure == after.workflow.failure &&
        workflow.owner == after.workflow.owner && workflow.session_id == after.workflow.session_id &&
        workflow.accepted_samples == after.workflow.accepted_samples &&
        workflow.rejected_samples == after.workflow.rejected_samples &&
        workflow.last_activity_us == after.workflow.last_activity_us &&
        durable == after.durable && calls == after.calls && writes == after.writes;
  }
};
calibration::CalibrationCommandEnvelope status_command() {
  calibration::CalibrationCommandEnvelope status{};
  status.operation = calibration::CalibrationOperation::GetStatus;
  status.transport = calibration::CalibrationTransport::Https;
  status.device_id = ESP.getEfuseMac(); status.principal_id = 7U;
  status.request_id = 9001U; status.expires_at_us = 100000000U;
  return status;
}
int http(SigapApp& app) {
  auto& executor = DebugAppProbe::http(app);
  auto& controller = DebugAppProbe::controller(app);
  const auto status = status_command();
  calibration::CalibrationResultEnvelope status_result{};
  if (!controller.execute(status, 1000U, status_result) ||
      status_result.status != calibration::CalibrationCommandStatus::Accepted)
    return fail("real initialized authorizer/controller positive status");
  State before{};
  if (!before.capture(app)) return fail("nonempty state fixture");
  if (build::kSensorSimulation && !before.has_profile)
    return fail("SIM fixture must retain its actual nonempty DS TestOnly profile");
  network::DeviceControlCommand start{};
  if (!command("start", "valid-start", "{}", start)) return fail("valid canonical hashed command decode");
  network::DeviceControlResult result{};
  const bool accepted = executor.execute(start, 1000U, result);
  if (!build::kSensorSimulation) {
    if (!accepted || result.status != network::DeviceControlStatus::Accepted || !controller.active())
      return fail("OFF actual HTTPS executor START positive");
    network::DeviceControlCommand cancel{};
    if (!command("cancel", "valid-cancel", "{}", cancel) ||
        !executor.execute(cancel, 1001U, result) || controller.active())
      return fail("OFF HTTPS executor legitimate CANCEL cleanup");
    std::puts("PASS OFF canonical HTTPS decode/executor/controller START+CANCEL");
    return 0;
  }
  if (accepted || result.status != network::DeviceControlStatus::Failed)
    return fail("SIM first valid HTTPS START must be rejected");
  if (executor.execute(start, 1001U, result) || result.status != network::DeviceControlStatus::Failed)
    return fail("SIM rejected HTTPS START became successful replay");
  if (!before.unchanged(app)) return fail("SIM HTTPS START/retry touched state or replay persistence");
  constexpr const char* operations[] = {"criteria", "capture", "validate", "preview", "commit", "cancel"};
  constexpr const char* payloads[] = {"{\"samples\":5,\"max_span_counts\":1}", "{\"anchor\":\"tare\"}", "{}", "{}",
      "{\"expected_profile_hash\":\"sha256:1111111111111111111111111111111111111111111111111111111111111111\",\"expected_profile_version\":\"1\"}", "{}"};
  for (std::size_t i = 0U; i < std::size(operations); ++i) {
    network::DeviceControlCommand mutation{};
    if (!command(operations[i], "valid-mutation-" + std::to_string(i), payloads[i], mutation))
      return fail("mutation wire decode");
    if (executor.execute(mutation, 1002U, result) || !before.unchanged(app))
      return fail("SIM mutation changed actual adapter/controller state");
  }
  if (!controller.execute(status, 1003U, status_result) ||
      status_result.status != calibration::CalibrationCommandStatus::Replayed || !before.unchanged(app))
    return fail("nonempty successful replay entry was evicted/changed by denied mutations");
  std::puts("PASS SIM canonical HTTPS inputs rejected before replay/workflow/NVS; successful read-only replay retained");
  return 0;
}
int ble(SigapApp& app) {
  char output[384]{};
  std::size_t size{};
  State before{};
  if (!before.capture(app)) return fail("BLE nonempty state");
  const bool accepted = DebugAppProbe::ble(app).execute("CAL START hx711_organic", output, sizeof(output), size);
  if (!build::kSensorSimulation) {
    if (!accepted || !DebugAppProbe::controller(app).active()) return fail("OFF BLE START positive");
    if (!DebugAppProbe::ble(app).execute("CAL CANCEL hx711_organic", output, sizeof(output), size) ||
        DebugAppProbe::controller(app).active()) return fail("OFF BLE CANCEL cleanup");
  } else {
    if (accepted || std::string_view(output, size).find("status=ERROR") == std::string_view::npos ||
        !before.unchanged(app)) return fail("SIM BLE valid START changed business state");
  }
  std::puts("PASS BLE actual initialized adapter/controller admission");
  return 0;
}
int usb(SigapApp& app) {
  State before{};
  if (!before.capture(app)) return fail("USB nonempty state");
  test_platform::serial_input = "calibration start hx711_organic\n";
  test_platform::serial_output.clear();
  while (!test_platform::serial_input.empty()) DebugAppProbe::usb(app).poll(1000U);
  if (!build::kSensorSimulation) {
    calibration::CalibrationStatusSnapshot workflow{};
    if (!DebugAppProbe::workflow(app, workflow) || !workflow.active) return fail("OFF USB START positive");
    test_platform::serial_input = "calibration cancel\n";
    while (!test_platform::serial_input.empty()) DebugAppProbe::usb(app).poll(1001U);
    if (!DebugAppProbe::workflow(app, workflow) || workflow.active) return fail("OFF USB CANCEL cleanup");
  } else if (test_platform::serial_output.find("SENSOR_SIMULATION") == std::string::npos ||
             !before.unchanged(app)) return fail("SIM USB valid START changed state");
  std::puts("PASS USB actual maintenance parser/shared workflow admission");
  return 0;
}
}
int main(int argc, char** argv) {
  if (argc != 2) return fail("expected http/ble/usb");
  seed_test_boot();
  sigap::SigapApp app;
  app.setup();
  if (sigap::DebugAppProbe::controller(app).device_id() != ESP.getEfuseMac() ||
      !sigap::DebugAppProbe::config(app).start_draft()) return fail("initialized identity/draft fixture");
  if (std::strcmp(argv[1], "http") == 0) return http(app);
  if (std::strcmp(argv[1], "ble") == 0) return ble(app);
  if (std::strcmp(argv[1], "usb") == 0) return usb(app);
  return fail("unknown case");
}
