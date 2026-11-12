#pragma once

#include <array>
#include <string>
#include <vector>
#include <unity.h>
#include "../test_debug_admission/admission_fixtures.h"
#include "maintenance/ble_calibration_backend.h"
#include "network/device_control_codec.h"
#include "telemetry/sha256.h"

namespace calibration_admission_test {
using namespace sigap;
constexpr auto target = calibration::CalibrationTarget::Hx711Organic;
constexpr auto profile_target = acquisition::ProfileTarget::Hx711Organic;

inline std::string hex(const std::array<uint8_t, 32U>& bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (auto byte : bytes) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 15U]);
  }
  return result;
}

struct Clock final : maintenance::ICalibrationClock {
  uint64_t now{100U};
  uint64_t now_us() const noexcept override { return now; }
};
struct Evidence final : calibration::ICalibrationEvidenceSource {
  mutable unsigned calls{};
  uint64_t sequence{1U};
  int64_t counts{1000};
  bool latest(calibration::CalibrationTarget sensor, uint64_t after,
              calibration::TrialObservation& out, uint64_t& observed) const noexcept override {
    ++calls;
    if (sensor != target || sequence <= after) return false;
    out = calibration::Hx711Trial{counts, acquisition::SampleQuality::Uncalibrated, true, true};
    observed = sequence;
    return true;
  }
};
struct Authorizer final : calibration::ICalibrationTransportAuthorizer {
  mutable unsigned calls{};
  mutable uint64_t last_request{};
  bool allow{true};
  bool authorize(const calibration::CalibrationCommandEnvelope& cmd) const noexcept override {
    ++calls; last_request = cmd.request_id;
    return allow && cmd.device_id == 7U && cmd.transport != calibration::CalibrationTransport::Usb;
  }
};
struct Replay final : calibration::ICalibrationTransportReplayStore {
  mutable unsigned loads{}, prepared_loads{};
  unsigned prepares{}, saves{};
  mutable uint64_t last_request{};
  bool load(uint64_t id, calibration::CalibrationCommandEnvelope&,
            calibration::CalibrationResultEnvelope&) const noexcept override {
    ++loads; last_request = id; return false;
  }
  bool load_prepared(uint64_t, calibration::CalibrationCommandEnvelope&) const noexcept override {
    ++prepared_loads; return false;
  }
  bool prepare(const calibration::CalibrationCommandEnvelope&) noexcept override {
    ++prepares; return true;
  }
  bool save(const calibration::CalibrationCommandEnvelope&,
            const calibration::CalibrationResultEnvelope&) noexcept override {
    ++saves; return true;
  }
};

struct Fixture {
  admission_test::ConfigStore config_store{};
  config::ConfigManager config{config_store, {8ULL * 1024ULL * 1024ULL, 0U}};
  admission_test::Secrets secret_store{};
  provisioning::PersistentSecretStore secrets{secret_store};
  admission_test::Audit audit{};
  admission_test::Profiles profile_store{};
  acquisition::ProfileCodec codec{};
  acquisition::ProfilePersistence persistence{profile_store};
  acquisition::ProfileManager profiles{persistence, codec, &audit};
  Evidence evidence{};
  maintenance::CommandRouter router{config, secrets, &profiles, &evidence, nullptr, &audit};
  Clock clock{};
  Authorizer authorizer{};
  Replay replay{};
  calibration::CalibrationTransportController controller{router, 7U, &authorizer, &replay};
  maintenance::CommandRouterBleCalibrationBackend ble{router, clock, &controller};
  debug::RuntimeIoGate gate{};
  Fixture() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::ConfigBootResult::BootstrapDefaults),
                          static_cast<int>(config.begin()));
    TEST_ASSERT_TRUE(profiles.load(profile_target));
    router.set_calibration_transport_controller(&controller);
    router.set_io_gate(&gate);
    ble.set_io_gate(&gate);
  }
  std::vector<uint64_t> storage() const {
    return {config_store.calls.reads, config_store.calls.writes,
            secret_store.calls.reads, secret_store.calls.writes,
            profile_store.calls.reads, profile_store.calls.writes,
            replay.loads, replay.prepared_loads, replay.prepares, replay.saves};
  }
  std::vector<uint64_t> snapshot() const {
    auto out = storage();
    calibration::CalibrationStatusSnapshot status{};
    TEST_ASSERT_TRUE(router.status(status));
    out.insert(out.end(), {static_cast<uint64_t>(status.state), static_cast<uint64_t>(status.target),
      static_cast<uint64_t>(status.failure), status.active, status.session_id,
      status.accepted_samples, status.rejected_samples, status.last_activity_us,
      controller.active(), static_cast<uint64_t>(controller.owner()),
      profiles.generation(), static_cast<uint64_t>(profiles.state(profile_target)),
      audit.contexts, audit.profiles, audit.configs, audit.rollbacks, audit.last_context,
      evidence.calls, authorizer.calls, authorizer.last_request, replay.last_request});
    return out;
  }
  void reference() { evidence.counts = 2000; ++evidence.sequence; }
  calibration::CalibrationProfileRef proposal() {
    calibration::CalibrationProfileRef out{};
    TEST_ASSERT_TRUE(router.proposed_profile(out));
    TEST_ASSERT_TRUE(out.present);
    return out;
  }
  void committed(const calibration::CalibrationProfileRef& expected) {
    acquisition::ActiveProfileSnapshot active{};
    TEST_ASSERT_TRUE(profiles.snapshot(profile_target, active));
    TEST_ASSERT_TRUE(active.present);
    TEST_ASSERT_EQUAL_UINT32(1U, active.ref.profile_version);
    TEST_ASSERT_EQUAL_UINT64(expected.version, active.ref.profile_version);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.hash.data(), active.ref.hash.data(), expected.hash.size());
    acquisition::Hx711Profile value{};
    TEST_ASSERT_TRUE(codec.decode(active.encoded, value));
    TEST_ASSERT_EQUAL_INT64(1000, value.tare_offset_counts);
    TEST_ASSERT_FLOAT_WITHIN(0.001F, 1000.0F, value.counts_per_kg);
    TEST_ASSERT_FALSE(controller.active());
    TEST_ASSERT_EQUAL_UINT32(1U, audit.profiles);
  }
};
inline void same(const std::vector<uint64_t>& before, const std::vector<uint64_t>& after) {
  TEST_ASSERT_EQUAL_UINT32(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    const auto label = std::string("state/counter index ") + std::to_string(i);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before[i], after[i], label.c_str());
  }
}
inline bool ble(Fixture& f, std::string_view text, std::string* output = nullptr) {
  char bytes[1024]{};
  std::size_t size{};
  bool ok = f.ble.execute(text, bytes, sizeof(bytes), size);
  if (output != nullptr) output->assign(bytes, size);
  return ok;
}
inline void ble_step(Fixture& f, std::string_view text, bool held) {
  const auto previous_request = f.authorizer.last_request;
  if (held) {
    TEST_ASSERT_TRUE(f.gate.try_acquire(debug::IoOwner::DebugHttp));
    auto before = f.snapshot();
    for (unsigned i = 0; i < 2U; ++i) {
      TEST_ASSERT_FALSE(ble(f, text));
      same(before, f.snapshot());
      TEST_ASSERT_TRUE(f.gate.active(debug::IoOwner::DebugHttp));
    }
    f.gate.release(debug::IoOwner::DebugHttp);
  }
  TEST_ASSERT_TRUE_MESSAGE(ble(f, text), std::string(text).c_str());
  TEST_ASSERT_EQUAL_UINT64(previous_request + 1U, f.authorizer.last_request);
  ++f.clock.now;
}
inline std::string ble_commit(const calibration::CalibrationProfileRef& profile) {
  return "CAL COMMIT hx711_organic expected_profile_version=" + std::to_string(profile.version) +
         " expected_profile_hash=sha256:" + hex(profile.hash);
}
inline std::string commit_payload(const calibration::CalibrationProfileRef& profile) {
  return "{\"expected_profile_version\":\"" + std::to_string(profile.version) +
         "\",\"expected_profile_hash\":\"sha256:" + hex(profile.hash) + "\"}";
}

// Mirror the Go producer's field order; fixed UTC is audit metadata, not MCU time.
inline std::string request_hash(std::string_view operation, std::string_view payload,
                                std::string_view session = "session-1") {
  const auto identity = std::string("{\"device_id\":\"0000000000000007\",\"operator_id\":\"operator-1\",\"session_id\":\"") +
    std::string(session) + "\",\"sensor_id\":\"hx711_organic\",\"operation\":\"" + std::string(operation) +
    "\",\"payload\":" + std::string(payload) + ",\"expires_at\":\"1970-01-01T00:00:01Z\"}";
  return "sha256:" + hex(telemetry_contract::sha256(identity));
}
inline network::DeviceControlCommand https_command(std::string_view operation,
    std::string_view payload, std::string_view id, std::string_view session = "session-1") {
  const auto wire = std::string("{\"command_id\":\"") + std::string(id) +
    "\",\"device_id\":\"0000000000000007\",\"operator_id\":\"operator-1\",\"session_id\":\"" +
    std::string(session) + "\",\"sensor_id\":\"hx711_organic\",\"operation\":\"" + std::string(operation) +
    "\",\"payload\":" + std::string(payload) + ",\"expires_in_ms\":1000,\"attempt\":1,\"request_hash\":\"" +
    request_hash(operation, payload, session) + "\",\"expires_at\":\"1970-01-01T00:00:01Z\"}";
  network::DeviceControlCommand out{};
  TEST_ASSERT_TRUE(network::decode_device_control_command(wire, 0U, out));
  return out;
}
}
