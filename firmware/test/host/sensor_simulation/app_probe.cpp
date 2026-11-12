#include "sigap_app.h"
#include "fixtures.h"
#include <cstdio>
#include <cstring>

namespace sigap::board { void log_runtime_board_memory() noexcept {} }
namespace sigap {
struct DebugAppProbe {
  static const acquisition::Ds18b20Profile* ds(SigapApp& app) { return app.profiles_.ds18b20; }
  static maintenance::CommandRouter& router(SigapApp& app) { return app.maintenance_router_; }
  static maintenance::CommandRouterBleCalibrationBackend& ble(SigapApp& app) { return app.ble_calibration_backend_; }
  static calibration::CalibrationTransportController& controller(SigapApp& app) { return app.calibration_controller_; }
  static maintenance::MaintenanceService& maintenance(SigapApp& app) { return app.maintenance_service_; }
  static config::ConfigManager& config(SigapApp& app) { return app.config_manager_; }
  static acquisition::ProfileManager& profiles(SigapApp& app) { return app.profile_manager_; }
  static std::size_t audit_size(SigapApp& app) { return app.audit_ring_.size(); }
  static void poll_acquisition(SigapApp& app, uint64_t now) { app.acquisition_.poll(now); }
  static bool pop_sample(SigapApp& app, acquisition::SampleRecord& out) {
    return app.acquisition_.handoff().try_pop(out) == acquisition::HandoffStatus::Dequeued;
  }
};
}
int failure(const char* message) { std::fprintf(stderr, "FAIL %s\n", message); return 1; }
struct TestHeaterRail final : sigap::acquisition::IMq7HeaterRail {
  bool high_ok{true}, low_ok{true};
  bool select_high() noexcept override { return high_ok; }
  bool select_low() noexcept override { return low_ok; }
};
struct TestMqReader final : sigap::acquisition::IMqAdcReader {
  sigap::acquisition::MqAdcReadResult read() noexcept override {
    return {true, false, 123U, sigap::acquisition::SampleReason::DataReady};
  }
};
bool phase_record(sigap::acquisition::Mq7HeaterController& heater,
                  TestMqReader& reader, sigap::acquisition::RecordFactory& factory,
                  uint64_t now, sigap::acquisition::Mq7HeaterPhase expected) {
  sigap::acquisition::MqAdcAcquirer acquirer(
      sigap::acquisition::SensorId::Mq7, sigap::acquisition::StreamId::B3,
      reader, factory, nullptr, &heater);
  sigap::acquisition::SampleRecord record{};
  return acquirer.poll(now, record) == sigap::acquisition::DriverPollStatus::Produced &&
      record.raw.heater_phase_available && record.raw.heater_phase == expected;
}
int main(int argc, char** argv) {
  if (argc != 2) return failure("expected profile or calibration");
  seed_test_boot();
  sigap::SigapApp app;
  app.setup();
  if (!app.acquisition().enabled()) return failure("normal configured app acquisition disabled");
  if (std::strcmp(argv[1], "phase") == 0) {
    sigap::acquisition::SequenceAllocator sequence;
    sigap::acquisition::BootId boot{}; boot.present = true;
    sigap::acquisition::DeviceId device{}; device.present = true;
    sigap::acquisition::RecordFactory factory(sequence,
        {boot, device, sigap::acquisition::ConfigRef{}, nullptr});
    TestHeaterRail rail;
    sigap::acquisition::Mq7HeaterProfile profile{};
    profile.preheat_us = profile.high_clean_us = profile.low_detect_us = 100U;
    sigap::acquisition::Mq7HeaterController heater(rail, profile);
    TestMqReader reader;
    heater.start(0U);
    if (!phase_record(heater, reader, factory, 1U, sigap::acquisition::Mq7HeaterPhase::Preheat)) return failure("PREHEAT phase lost");
    heater.tick(100U);
    if (!phase_record(heater, reader, factory, 101U, sigap::acquisition::Mq7HeaterPhase::HighClean)) return failure("HIGH_CLEAN phase lost");
    heater.tick(200U);
    if (!phase_record(heater, reader, factory, 201U, sigap::acquisition::Mq7HeaterPhase::LowDetect)) return failure("LOW_DETECT phase lost");
    heater.tick(300U);
    if (!phase_record(heater, reader, factory, 301U, sigap::acquisition::Mq7HeaterPhase::HighClean)) return failure("cycle phase lost");
    rail.low_ok = false;
    heater.start(400U); heater.tick(500U); heater.tick(600U);
    if (!phase_record(heater, reader, factory, 601U, sigap::acquisition::Mq7HeaterPhase::Fault)) return failure("FAULT phase lost");
    std::puts("PASS MQ-7 producer preserves all four controller states");
    return 0;
  }
  if (std::strcmp(argv[1], "profile") == 0) {
    const auto* profile = sigap::DebugAppProbe::ds(app);
    if (profile == nullptr || profile->manifest.validity != sigap::acquisition::ProfileValidity::TestOnly)
      return failure("SIM app must provide explicit RAM-only DS protocol fixture");
    sigap::acquisition::EncodedProfile encoded;
    if (!sigap::acquisition::ProfileCodec{}.encode(*profile, encoded) ||
        encoded.hash() != profile->manifest.hash)
      return failure("virtual profile must carry actual canonical codec hash");
    if (!profile->valid_for_acquisition()) return failure("virtual DS profile not usable");
    for (const auto& ticks : std::array<std::array<uint16_t, 2>, 2>{{
             {{0U, 65535U}}, {{65535U, 0U}}}}) {
      std::array<uint8_t, 6> frame{{static_cast<uint8_t>(ticks[0] >> 8U),
          static_cast<uint8_t>(ticks[0]), 0U, static_cast<uint8_t>(ticks[1] >> 8U),
          static_cast<uint8_t>(ticks[1]), 0U}};
      frame[2] = sigap::acquisition::sht31_crc8(frame.data(), 2U);
      frame[5] = sigap::acquisition::sht31_crc8(frame.data() + 3U, 2U);
      const auto decoded = sigap::acquisition::decode_sht31_frame(frame);
      const int32_t temp = static_cast<int32_t>(
          (-4500LL * 65535LL + 17500LL * ticks[0]) / 65535LL);
      const int32_t humidity = static_cast<int32_t>((10000LL * ticks[1]) / 65535LL);
      if (!decoded.temperature_crc_valid || !decoded.humidity_crc_valid ||
          decoded.temperature_centi_c != temp || decoded.humidity_centi_pct != humidity)
        return failure("SHT31 tick extrema/CRC conversion fixture");
    }
    std::array<unsigned, 13> seen{};
    for (uint64_t now = 0U; now <= 6200000U; now += 1000U) {
      test_platform::now_us = now;
      sigap::DebugAppProbe::poll_acquisition(app, now);
      sigap::acquisition::SampleRecord sample{};
      while (sigap::DebugAppProbe::pop_sample(app, sample)) {
        const auto index = static_cast<std::size_t>(sample.header.sensor);
        if (index >= seen.size()) return failure("full-app producer sensor identity");
        sigap::storage::ImmutableRecordBytes encoded{};
        sigap::acquisition::SampleRecord restored{};
        if (!sigap::storage::RecordCodec::encode_sample(sample, encoded) ||
            !sigap::storage::RecordCodec::decode_sample(encoded, restored))
          return failure("full-app sample durable codec roundtrip");
        sigap::telemetry_archive::ImmutableBatch batch{};
        if (!sigap::telemetry_archive::BatchJsonBuilder{}.build(encoded, batch))
          return failure("actual telemetry-v3 builder rejected full-app sample");
        const auto json = batch.exact_body();
        if (index == static_cast<std::size_t>(sigap::acquisition::SensorId::Mq7) &&
            json.find("\"heater_phase\":\"FAULT\"") == std::string_view::npos)
          return failure("MQ-7 controller phase missing from telemetry-v3");
        if (index == static_cast<std::size_t>(sigap::acquisition::SensorId::Ds18b20B3) &&
            (json.find("\"rom\"") == std::string_view::npos ||
             json.find("\"scratchpad_hex\"") == std::string_view::npos ||
             !restored.raw.identity_valid || restored.raw.ds_rom != profile->rom ||
             sigap::acquisition::ds18b20_crc8(restored.raw.ds_rom.data(), 7U) != restored.raw.ds_rom[7] ||
             restored.raw.ds_scratchpad[8] != sigap::acquisition::ds18b20_crc8(
                 restored.raw.ds_scratchpad.data(), 8U) ||
             restored.raw.ds_resolution_bits != profile->resolution_bits))
          return failure("generated DS18B20 ROM/frame CRC/resolution/identity absent from telemetry-v3");
        if (index == static_cast<std::size_t>(sigap::acquisition::SensorId::Sht31_44) ||
            index == static_cast<std::size_t>(sigap::acquisition::SensorId::Sht31_45)) {
          const std::string temp = "\"temperature_ticks\":" + std::to_string(restored.raw.sht31_temperature_ticks);
          const std::string humidity = "\"humidity_ticks\":" + std::to_string(restored.raw.sht31_humidity_ticks);
          const int32_t expected_temp = static_cast<int32_t>(
              (-4500LL * 65535LL + 17500LL * restored.raw.sht31_temperature_ticks) / 65535LL);
          const int32_t expected_humidity = static_cast<int32_t>(
              (10000LL * restored.raw.sht31_humidity_ticks) / 65535LL);
          if (!restored.raw.sht31_ticks_available || !restored.raw.crc_valid ||
              !restored.raw.temperature_crc_valid || !restored.raw.humidity_crc_valid ||
              restored.raw.temperature_centi_c != expected_temp ||
              restored.raw.humidity_centi_pct != expected_humidity ||
              json.find(temp) == std::string_view::npos || json.find(humidity) == std::string_view::npos)
            return failure("SHT31 address frame CRC/ticks/inverse conversion mismatch");
        }
        if (index == static_cast<std::size_t>(sigap::acquisition::SensorId::Mq7) ||
            index == static_cast<std::size_t>(sigap::acquisition::SensorId::Sht31_44)) {
          sigap::storage::ArchiveRecordView view{};
          sigap::storage::ImmutableRecordBytes legacy{};
          sigap::storage::PayloadWriter writer{nullptr, 0U};
          sigap::acquisition::SampleRecord old_sample{};
          sigap::telemetry_archive::ImmutableBatch rejected{};
          if (!sigap::storage::RecordCodec::read(encoded, view) ||
              view.payload_size <= sigap::storage::kArchiveSampleExtensionBytes ||
              !sigap::storage::RecordCodec::begin(view.identity, legacy, writer) ||
              !writer.bytes(view.payload, view.payload_size - sigap::storage::kArchiveSampleExtensionBytes) ||
              !sigap::storage::RecordCodec::finish(legacy, writer) ||
              !sigap::storage::RecordCodec::decode_sample(legacy, old_sample))
            return failure("construct/decode actual legacy sample payload");
          const bool available = index == static_cast<std::size_t>(sigap::acquisition::SensorId::Mq7)
              ? old_sample.raw.heater_phase_available : old_sample.raw.sht31_ticks_available;
          if (available || sigap::telemetry_archive::BatchJsonBuilder{}.build(legacy, rejected))
            return failure("legacy absent phase/ticks must not be inferred or degraded");
        }
        ++seen[index];
      }
    }
    for (const auto count : seen) if (count == 0U) return failure("full-app producer omitted sensor");
    std::puts("PASS normal SIM app DS TestOnly protocol profile/hash");
    return 0;
  }
  using namespace sigap::maintenance;
  if (std::strcmp(argv[1], "transports") == 0) {
    auto& router = sigap::DebugAppProbe::router(app);
    test_platform::serial_output.clear();
    test_platform::serial_input = "status\n";
    auto& usb = sigap::DebugAppProbe::maintenance(app);
    while (!test_platform::serial_input.empty()) usb.poll(1000U);
    if (test_platform::serial_output.find("mode=") == std::string::npos)
      return failure("USB admitted read-only status control unavailable in SIM");
    char status_response[384]{};
    std::size_t status_size{};
    if (!sigap::DebugAppProbe::ble(app).execute("CAL STATUS", status_response,
                                               sizeof(status_response), status_size) ||
        std::string_view(status_response, status_size).find("status=ACCEPTED") == std::string_view::npos)
      return failure("BLE admitted read-only status control unavailable in SIM");
    // Allowed read-only controls may consult persistence; snapshot denied mutations afterward.
    const auto writes = test_platform::nvs_writes;
    const auto nvs_calls_before = test_platform::nvs_calls;
    const auto durable_records_before = test_platform::nvs;
    if (durable_records_before.empty())
      return failure("SIM test requires nonempty mock durable state before adapter calls");
    sigap::config::EncodedConfig active_before{}, draft_before{};
    const auto& config = sigap::DebugAppProbe::config(app);
    if (!sigap::config::ConfigCodec{}.encode(config.active(), active_before) ||
        !sigap::config::ConfigCodec{}.encode(config.draft(), draft_before))
      return failure("capture pre-ingress SIM config");
    const bool had_draft_before = config.has_draft();
    const auto generation_before = sigap::DebugAppProbe::profiles(app).generation();
    const auto audit_before = sigap::DebugAppProbe::audit_size(app);
    const auto active_calibration_before = sigap::DebugAppProbe::controller(app).active();
    sigap::acquisition::ActiveProfileSnapshot profile_before{};
    const bool profile_present_before = sigap::DebugAppProbe::profiles(app).snapshot(
        sigap::acquisition::ProfileTarget::Hx711Organic, profile_before);
    test_platform::serial_output.clear();
    test_platform::serial_input =
        "calibration start hx711_organic\ncalibration criteria samples 5\n"
        "calibration capture hx711_organic\ncalibration validate\n"
        "calibration preview\ncalibration commit\n"
        "profile import hx711_organic\n"
        "profile import begin hx711_organic 8 deadbeef\n"
        "profile import chunk 0 deadbeef\n"
        "profile import finish hx711_organic\nprofile validate hx711_organic\n"
        "profile preview hx711_organic\nprofile commit hx711_organic\n"
        "profile rollback hx711_organic\nprofile reset hx711_organic CONFIRM\n";
    while (!test_platform::serial_input.empty()) usb.poll(1000U);
    std::size_t usb_rejections{};
    for (std::size_t at = 0U; (at = test_platform::serial_output.find("SENSOR_SIMULATION", at)) != std::string::npos; ++at)
      ++usb_rejections;
    if (usb_rejections != 15U)
      return failure("USB maintenance decoder/dispatch must reject simulated mutation");

    char ble_response[384]{};
    std::size_t ble_size{};
    const char* ble_commands[] = {
        "CAL START hx711_organic",
        "CAL CRITERIA hx711_organic samples=5 max_span_counts=1",
        "CAL CAPTURE hx711_organic tare", "CAL VALIDATE hx711_organic",
        "CAL PREVIEW hx711_organic",
        "CAL COMMIT hx711_organic expected_profile_version=1 expected_profile_hash=sha256:0000000000000000000000000000000000000000000000000000000000000000"};
    for (const char* command : ble_commands) {
      ble_size = 0U;
      if (sigap::DebugAppProbe::ble(app).execute(command, ble_response,
                                                 sizeof(ble_response), ble_size) ||
          std::string_view(ble_response, ble_size).find("status=ERROR") == std::string_view::npos)
        return failure("BLE calibration adapter must reject every simulated workflow operation");
    }

    sigap::ApplicationCalibrationExecutor https(
        router, sigap::DebugAppProbe::controller(app));
    const char* operations[] = {"start", "capture", "validate", "preview", "commit"};
    const char* payloads[] = {"{}", "{\"anchor\":\"tare\"}", "{}", "{}",
                              "{\"expected_profile_hash\":\"sha256:0000000000000000000000000000000000000000000000000000000000000000\",\"expected_profile_version\":1}"};
    for (std::size_t i = 0U; i < 5U; ++i) {
      sigap::network::DeviceControlCommand command{};
      (void)command.command_id.assign("host-f2-" + std::to_string(i));
      (void)command.device_id.assign("1234abcdef56");
      (void)command.operator_id.assign("host-operator");
      (void)command.session_id.assign("host-session");
      (void)command.sensor_id.assign("hx711_organic");
      (void)command.operation.assign(operations[i]);
      (void)command.request_hash.assign("sha256:0000000000000000000000000000000000000000000000000000000000000000");
      command.expires_at_us = 100000000U;
      command.payload_size = std::strlen(payloads[i]);
      std::memcpy(command.payload.data(), payloads[i], command.payload_size);
      sigap::network::DeviceControlResult result{};
      if (https.execute(command, 1000U, result) || result.status != sigap::network::DeviceControlStatus::Failed)
        return failure("HTTPS device-control executor must reject simulated calibration operation");
    }
    if (test_platform::nvs_writes != writes)
      return failure("USB/BLE/HTTPS simulated calibration/profile rejection wrote NVS");
    sigap::config::EncodedConfig active_after{}, draft_after{};
    sigap::acquisition::ActiveProfileSnapshot profile_after{};
    if (!sigap::config::ConfigCodec{}.encode(config.active(), active_after) ||
        !sigap::config::ConfigCodec{}.encode(config.draft(), draft_after) ||
        active_after.bytes != active_before.bytes || draft_after.bytes != draft_before.bytes ||
        config.has_draft() != had_draft_before ||
        sigap::DebugAppProbe::profiles(app).generation() != generation_before ||
        sigap::DebugAppProbe::audit_size(app) != audit_before ||
        sigap::DebugAppProbe::controller(app).active() != active_calibration_before ||
        sigap::DebugAppProbe::profiles(app).snapshot(
            sigap::acquisition::ProfileTarget::Hx711Organic, profile_after) != profile_present_before ||
        (profile_present_before && profile_after.encoded.hash() != profile_before.encoded.hash()) ||
        test_platform::nvs_calls != nvs_calls_before ||
        test_platform::nvs != durable_records_before)
      return failure("SIM transport rejection changed RAM config/profile/draft/session/audit state");
    std::puts("PASS actual host adapters USB mutations=15 BLE operations=6 HTTPS operations=5 persistence writes=0");
    return 0;
  }
  auto& router = sigap::DebugAppProbe::router(app);
  const auto writes = test_platform::nvs_writes;
  const CommandKind mutations[] = {
    CommandKind::CalibrationStart, CommandKind::CalibrationCriteria,
    CommandKind::CalibrationCapture, CommandKind::CalibrationValidate,
    CommandKind::CalibrationPreview, CommandKind::CalibrationCommit,
    CommandKind::CalibrationCancel, CommandKind::ProfileImport,
    CommandKind::ProfileImportBegin, CommandKind::ProfileImportChunk,
    CommandKind::ProfileImportFinish, CommandKind::ProfileValidate,
    CommandKind::ProfilePreview, CommandKind::ProfileCommit,
    CommandKind::ProfileRollback, CommandKind::ProfileReset,
  };
  for (const auto kind : mutations) {
    MaintenanceCommand command{}; command.kind = kind; command.target.assign("hx711_organic");
    MaintenanceResponse response;
    if (router.execute(command, 1000U, response) != CommandStatus::Rejected ||
        response.view().find("SENSOR_SIMULATION") == std::string_view::npos)
      return failure("simulation must reject common sensor calibration/profile mutations");
  }
  MaintenanceCommand status{}; status.kind = CommandKind::Status;
  MaintenanceResponse response;
  if (router.execute(status, 1000U, response) != CommandStatus::Accepted)
    return failure("simulation must preserve read-only status");
  if (test_platform::nvs_writes != writes) return failure("simulation calibration caused NVS writes");
  std::puts("PASS SIM common calibration/profile mutation guard cases=16 writes=0 status=available");
  return 0;
}
