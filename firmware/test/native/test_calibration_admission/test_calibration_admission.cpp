#include <algorithm>
#include <string_view>
#include <unity.h>
#include "config/config_manager.h"
#include "config/config_store.h"
#include "config/config_validator.h"
#include "maintenance/runtime.h"
#include "provisioning/secret_store.h"
#include "debug/io_gate.h"
#include "calibration_fixtures.h"
#define ARDUINO
#include "../../../src/app/device_control_executor.cpp"
#undef ARDUINO

namespace {
constexpr sigap::config::ValidationContext kContext{8ULL * 1024ULL * 1024ULL, 0U};
class Authorizer final : public sigap::calibration::ICalibrationTransportAuthorizer {
 public:
  bool authorize(const sigap::calibration::CalibrationCommandEnvelope& command) const noexcept override {
    return command.transport == sigap::calibration::CalibrationTransport::Https;
  }
};

class ReplayStore final : public sigap::calibration::ICalibrationTransportReplayStore {
 public:
  bool prepare(const sigap::calibration::CalibrationCommandEnvelope&) noexcept override {
    ++prepares;
    return true;
  }
  bool load(uint64_t request_id, sigap::calibration::CalibrationCommandEnvelope&,
            sigap::calibration::CalibrationResultEnvelope&) const noexcept override {
    ++loads; last_request_id = request_id; return false;
  }
  bool load_prepared(uint64_t, sigap::calibration::CalibrationCommandEnvelope&) const noexcept override {
    ++prepared_loads; return false;
  }
  bool save(const sigap::calibration::CalibrationCommandEnvelope&,
            const sigap::calibration::CalibrationResultEnvelope&) noexcept override {
    ++saves; return true;
  }
  mutable unsigned loads{0U};
  mutable unsigned prepared_loads{0U};
  unsigned prepares{0U};
  unsigned saves{0U};
  mutable uint64_t last_request_id{0U};
};

sigap::network::DeviceControlCommand start_command() {
  sigap::network::DeviceControlCommand command{};
  command.command_id.assign("command-1");
  command.device_id.assign("0000000000000007");
  command.operator_id.assign("operator-1");
  command.session_id.assign("session-1");
  command.sensor_id.assign("hx711_organic");
  command.operation.assign("start");
  command.request_hash.assign("sha256:3d7bf5e5feec57e1ae3dfdabf719cbfc76835da7879dbed6ac211438e3559627");
  constexpr std::string_view payload{"{\"reference_mass_mg\":2000}"};
  std::copy(payload.begin(), payload.end(), command.payload.begin());
  command.payload_size = payload.size();
  command.expires_at_us = 1000000U;
  return command;
}

void test_actual_https_executor_denies_before_controller_and_setup_then_admits() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config(config_store, kContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
                        static_cast<int>(config.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_store{};
  sigap::provisioning::PersistentSecretStore secrets(secret_store);
  sigap::maintenance::CommandRouter router(config, secrets);
  Authorizer authorizer{};
  ReplayStore replay{};
  sigap::calibration::CalibrationTransportController controller(router, 7U, &authorizer, &replay);
  router.set_calibration_transport_controller(&controller);
  sigap::ApplicationCalibrationExecutor executor(router, controller);
  sigap::debug::RuntimeIoGate gate{};
  executor.set_io_gate(&gate);
  auto command = start_command();
  sigap::network::DeviceControlResult result{};

  TEST_ASSERT_TRUE(gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  TEST_ASSERT_FALSE(executor.execute(command, 100U, result));
  TEST_ASSERT_FALSE(controller.active());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::network::DeviceControlStatus::Failed),
                        static_cast<int>(result.status));
  gate.release(sigap::debug::IoOwner::DebugHttp);
  const bool admitted = executor.execute(command, 100U, result);
  TEST_ASSERT_TRUE(admitted);
  TEST_ASSERT_TRUE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(1U, replay.loads);
  const unsigned loads_before_cancel = replay.loads;
  const unsigned prepared_before_cancel = replay.prepared_loads;
  const unsigned prepares_before_cancel = replay.prepares;
  const unsigned saves_before_cancel = replay.saves;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::network::DeviceControlStatus::Accepted),
                        static_cast<int>(result.status));

  command.command_id.assign("command-2");
  command.operation.assign("cancel");
  constexpr std::string_view payload{"{}"};
  std::copy(payload.begin(), payload.end(), command.payload.begin());
  command.payload_size = payload.size();
  command.request_hash.assign("sha256:25f76f061d156779df20db575aff11c2a466b6dd025d79a7fd09c0d146e76189");
  TEST_ASSERT_TRUE(gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  TEST_ASSERT_FALSE(executor.execute(command, 200U, result));
  TEST_ASSERT_TRUE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(loads_before_cancel, replay.loads);
  TEST_ASSERT_EQUAL_UINT32(prepared_before_cancel, replay.prepared_loads);
  TEST_ASSERT_EQUAL_UINT32(prepares_before_cancel, replay.prepares);
  TEST_ASSERT_EQUAL_UINT32(saves_before_cancel, replay.saves);
  gate.release(sigap::debug::IoOwner::DebugHttp);
  TEST_ASSERT_TRUE(executor.execute(command, 200U, result));
  TEST_ASSERT_FALSE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(2U, replay.loads);
}
}  // namespace

namespace ca = calibration_admission_test;
struct HttpsFixture : ca::Fixture {
  sigap::ApplicationCalibrationExecutor executor{router, controller};
  HttpsFixture() { executor.set_io_gate(&gate); }
  void replay_command(const sigap::network::DeviceControlCommand& cmd,
                      const sigap::network::DeviceControlResult& expected) {
    const auto before = storage();
    const auto contexts = audit.contexts;
    const auto samples = evidence.calls;
    sigap::network::DeviceControlResult out{};
    TEST_ASSERT_TRUE(executor.execute(cmd, clock.now, out));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected.status), static_cast<int>(out.status));
    TEST_ASSERT_EQUAL_UINT32(expected.payload_size, out.payload_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.payload.data(), out.payload.data(), expected.payload_size);
    TEST_ASSERT_EQUAL_UINT32(contexts, audit.contexts);
    TEST_ASSERT_EQUAL_UINT32(samples, evidence.calls);
    ca::same(before, storage());
  }
  sigap::network::DeviceControlResult step(const sigap::network::DeviceControlCommand& cmd, bool held) {
    sigap::network::DeviceControlResult out{};
    if (held) {
      TEST_ASSERT_TRUE(gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
      const auto before = snapshot();
      for (unsigned i = 0U; i < 2U; ++i) {
        TEST_ASSERT_FALSE(executor.execute(cmd, clock.now, out));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::network::DeviceControlStatus::Failed), static_cast<int>(out.status));
        ca::same(before, snapshot());
        TEST_ASSERT_TRUE(gate.active(sigap::debug::IoOwner::DebugHttp));
      }
      gate.release(sigap::debug::IoOwner::DebugHttp);
    }
    TEST_ASSERT_TRUE_MESSAGE(executor.execute(cmd, clock.now, out), std::string(cmd.operation.view()).c_str());
    TEST_ASSERT_EQUAL_STRING(std::string(cmd.command_id.view()).c_str(), std::string(out.command_id.view()).c_str());
    ++clock.now;
    return out;
  }
};
std::string result_bytes(const sigap::network::DeviceControlResult& result) {
  return {reinterpret_cast<const char*>(result.payload.data()), result.payload_size};
}
sigap::calibration::CalibrationProfileRef https_preview(HttpsFixture& f, bool held) {
  const auto run = [&](const char* operation, const char* payload, const char* id) {
    const auto cmd = ca::https_command(operation, payload, id);
    const auto result = f.step(cmd, held);
    f.replay_command(cmd, result);
    return result;
  };
  run("start", "{\"reference_mass_mg\":1000000}", "start");
  run("set_criteria", "{\"samples\":1,\"max_span_counts\":10}", "criteria");
  run("capture", "{\"anchor\":\"tare\"}", "tare");
  f.reference();
  run("capture", "{\"anchor\":\"reference\"}", "reference");
  run("validate", "{}", "validate");
  const auto preview = run("preview", "{}", "preview");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::network::DeviceControlStatus::PreviewReady), static_cast<int>(preview.status));
  return f.proposal();
}
void https_workflow(bool held) {
  HttpsFixture f;
  const auto proposed = https_preview(f, held);
  const auto commit = ca::https_command("commit", ca::commit_payload(proposed), "commit");
  const auto first = f.step(commit, held);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::network::DeviceControlStatus::Committed), static_cast<int>(first.status));
  f.committed(proposed);
  const auto writes = f.profile_store.calls.writes;
  const auto replay = f.step(commit, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::network::DeviceControlStatus::Committed), static_cast<int>(replay.status));
  TEST_ASSERT_EQUAL_STRING(result_bytes(first).c_str(), result_bytes(replay).c_str());
  TEST_ASSERT_EQUAL_UINT32(writes, f.profile_store.calls.writes);
  TEST_ASSERT_EQUAL_UINT32(1U, f.audit.profiles);
  auto wrong = proposed; ++wrong.version;
  auto conflicting = ca::https_command("commit", ca::commit_payload(wrong), "commit");
  sigap::network::DeviceControlResult out{};
  TEST_ASSERT_FALSE(f.executor.execute(conflicting, f.clock.now, out));
  TEST_ASSERT_EQUAL_UINT32(writes, f.profile_store.calls.writes);
}
void test_https_full_actual_profile_workflow_and_exact_commit_replay() { https_workflow(false); }
void test_https_each_valid_step_denies_twice_without_effects_then_releases() { https_workflow(true); }
void test_https_canonical_identity_and_nonempty_payload_use_actual_network_decoder() {
  const auto start = ca::https_command("start", "{\"reference_mass_mg\":2000}", "command-1");
  TEST_ASSERT_EQUAL_STRING("sha256:3d7bf5e5feec57e1ae3dfdabf719cbfc76835da7879dbed6ac211438e3559627", std::string(start.request_hash.view()).c_str());
  const auto cancel = ca::https_command("cancel", "{}", "command-2");
  TEST_ASSERT_EQUAL_STRING("sha256:25f76f061d156779df20db575aff11c2a466b6dd025d79a7fd09c0d146e76189", std::string(cancel.request_hash.view()).c_str());
  TEST_ASSERT_EQUAL_UINT32(2U, cancel.payload_size);
}
void test_https_replayed_start_does_not_reapply_setup_or_renew_activity() {
  HttpsFixture f;
  const auto cmd = ca::https_command("start", "{\"reference_mass_mg\":1000000}", "start");
  f.step(cmd, false);
  sigap::calibration::CalibrationStatusSnapshot before{}, after{};
  TEST_ASSERT_TRUE(f.router.status(before));
  const auto storage = f.storage();
  const auto contexts = f.audit.contexts;
  f.clock.now = 500U;
  f.step(cmd, false);
  TEST_ASSERT_TRUE(f.router.status(after));
  TEST_ASSERT_EQUAL_UINT64(before.last_activity_us, after.last_activity_us);
  TEST_ASSERT_EQUAL_UINT32(contexts, f.audit.contexts);
  ca::same(storage, f.storage());
}
void test_https_expected_profile_conflict_preserves_session_for_correct_commit() {
  HttpsFixture f;
  const auto correct = https_preview(f, false);
  auto wrong = correct; ++wrong.version;
  const auto writes = f.profile_store.calls.writes;
  const auto cmd = ca::https_command("commit", ca::commit_payload(wrong), "wrong-version");
  sigap::network::DeviceControlResult out{};
  TEST_ASSERT_FALSE(f.executor.execute(cmd, f.clock.now, out));
  TEST_ASSERT_TRUE(f.controller.active());
  TEST_ASSERT_EQUAL_UINT32(writes, f.profile_store.calls.writes);
  f.step(ca::https_command("commit", ca::commit_payload(correct), "correct-version"), false);
  f.committed(correct);
}
void test_https_cancel_busy_has_no_effects_then_retires_the_owner_after_release() {
  HttpsFixture f;
  f.step(ca::https_command("start", "{}", "start"), false);
  f.step(ca::https_command("cancel", "{}", "cancel"), true);
  TEST_ASSERT_FALSE(f.controller.active());
  TEST_ASSERT_EQUAL_UINT32(0U, f.profile_store.calls.writes);
}
void test_https_expiry_and_ble_disconnect_keep_https_owner_isolated() {
  HttpsFixture f;
  f.step(ca::https_command("start", "{}", "start"), false);
  const auto before = f.snapshot();
  f.ble.disconnect();
  ca::same(before, f.snapshot());
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto storage = f.storage();
  f.clock.now += sigap::calibration::CalibrationSession::kSessionTimeoutUs + 1U;
  auto cancel = ca::https_command("cancel", "{}", "cancel");
  sigap::network::DeviceControlResult out{};
  TEST_ASSERT_FALSE(f.executor.execute(cancel, f.clock.now, out));
  TEST_ASSERT_TRUE(f.controller.active());
  f.controller.poll(f.clock.now);
  TEST_ASSERT_FALSE(f.controller.active());
  ca::same(storage, f.storage());
  TEST_ASSERT_TRUE(f.gate.active(sigap::debug::IoOwner::DebugHttp));
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  auto expired = ca::https_command("start", "{}", "expired");
  TEST_ASSERT_FALSE(f.executor.execute(expired, f.clock.now, out));
  TEST_ASSERT_FALSE(f.controller.active());
}
void test_https_malformed_fields_do_not_reach_controller_or_mutate_ownership() {
  for (const bool held : {false, true}) {
    HttpsFixture f;
    if (held) TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
    const auto before = f.snapshot();
    for (const auto& pair : std::array<std::pair<const char*, const char*>, 4>{{
        {"unknown", "{}"}, {"set_criteria", "{\"samples\":0,\"max_span_counts\":10}"},
        {"set_criteria", "{\"samples\":\"bad\",\"max_span_counts\":10}"},
        {"commit", "{\"expected_profile_version\":\"1\",\"expected_profile_hash\":\"bad\"}"}}}) {
      const auto cmd = ca::https_command(pair.first, pair.second, "invalid");
      sigap::network::DeviceControlResult out{};
      TEST_ASSERT_FALSE(f.executor.execute(cmd, f.clock.now, out));
      ca::same(before, f.snapshot());
    }
  }
}

void test_https_start_setup_validation_precedes_controller_and_partial_ownership() {
  for (const auto payload : {"{\"reference_mass_mg\":\"bad\"}", "{\"reference_mass_mg\":0}",
      "{\"reference_temperature_centi_c\":2147483648}", "{\"reference_humidity_centi_pct\":-1}",
      "{\"resolution_bits\":8}", "{\"echo_mm_per_us_milli\":1001}",
      "{\"echo_timeout_us\":0}", "{\"inter_sensor_gap_us\":4294967296}",
      "{\"overshoot_tolerance_mm_milli\":-1}", "{\"reference_mv\":0}",
      "{\"full_scale_counts\":0}", "{\"divider_numerator\":0}", "{\"divider_denominator\":0}"}) {
    HttpsFixture f;
    const auto before = f.snapshot();
    const auto cmd = ca::https_command("start", payload, "invalid-setup");
    sigap::network::DeviceControlResult out{};
    TEST_ASSERT_FALSE_MESSAGE(f.executor.execute(cmd, f.clock.now, out), payload);
    ca::same(before, f.snapshot());
  }
}
void test_https_conflicting_transport_does_not_steal_ble_owner_and_stale_ble_cleanup_is_scoped() {
  HttpsFixture f;
  ca::ble_step(f, "CAL START hx711_organic", false);
  auto cmd = ca::https_command("start", "{}", "competing");
  sigap::network::DeviceControlResult out{};
  TEST_ASSERT_FALSE(f.executor.execute(cmd, f.clock.now, out));
  TEST_ASSERT_TRUE(f.controller.active());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::calibration::CalibrationTransport::Ble), static_cast<int>(f.controller.owner()));
  f.clock.now += sigap::calibration::CalibrationSession::kSessionTimeoutUs + 1U;
  f.controller.poll(f.clock.now);
  cmd.expires_at_us = f.clock.now + 1000000U;
  f.step(cmd, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::calibration::CalibrationTransport::Https), static_cast<int>(f.controller.owner()));
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto before = f.snapshot();
  f.ble.disconnect();
  ca::same(before, f.snapshot());
  TEST_ASSERT_TRUE(f.controller.active());
}

void test_https_setup_preflight_preserves_valid_router_boundaries() {
  for (const auto payload : {"{\"reference_mass_mg\":1}",
      "{\"reference_temperature_centi_c\":-2147483648}",
      "{\"reference_humidity_centi_pct\":0}", "{\"resolution_bits\":9}",
      "{\"overshoot_tolerance_mm_milli\":0}", "{\"echo_timeout_us\":4294967295}",
      "{\"inter_sensor_gap_us\":1}", "{\"reference_mv\":1}",
      "{\"full_scale_counts\":1}", "{\"divider_numerator\":1}", "{\"divider_denominator\":1}"}) {
    HttpsFixture f;
    f.step(ca::https_command("start", payload, "valid-setup"), false);
    TEST_ASSERT_TRUE(f.controller.active());
  }
}
void test_https_runtime_setup_failure_retires_only_its_new_session() {
  HttpsFixture f;
  // The evidence interface deliberately cannot apply this engineering conversion.
  const auto cmd = ca::https_command("start", "{\"echo_mm_per_us_milli\":171}", "setup-failure");
  sigap::network::DeviceControlResult out{};
  TEST_ASSERT_FALSE(f.executor.execute(cmd, f.clock.now, out));
  TEST_ASSERT_FALSE(f.controller.active());
  sigap::calibration::CalibrationStatusSnapshot status{};
  TEST_ASSERT_TRUE(f.router.status(status));
  TEST_ASSERT_FALSE(status.active);
  TEST_ASSERT_EQUAL_UINT32(0U, f.profile_store.calls.writes);
}

void test_https_duplicate_root_setup_keys_reject_before_controller_or_replay() {
  const std::array<std::pair<const char*, const char*>, 3> inputs{{
      {"{\"reference_mass_mg\":1000000}", "{\"reference_mass_mg\":1000000,\"reference_mass_mg\":2000000}"},
      {"{\"reference_temperature_centi_c\":0}", "{\"reference_temperature_centi_c\":0,\"reference_temperature_centi_c\":10}"},
      {"{\"reference_mass_mg\":1000000,\"reference_mv\":1}",
       "{\"reference_mass_mg\":1000000,\"reference_mv\":1,\"reference_mv\":2}"}}};
  for (const auto& input : inputs) {
    { HttpsFixture positive; positive.step(ca::https_command("start", input.first, "ambiguous"), false); }
    for (const bool held : {false, true}) {
      HttpsFixture f;
      if (held) TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
      const auto before = f.snapshot();
      // This goes through the real wire decoder: no manually forged command.
      const auto cmd = ca::https_command("start", input.second, "ambiguous");
      sigap::network::DeviceControlResult out{};
      TEST_ASSERT_FALSE(f.executor.execute(cmd, f.clock.now, out));
      ca::same(before, f.snapshot());
      if (held) {
        TEST_ASSERT_TRUE(f.gate.active(sigap::debug::IoOwner::DebugHttp));
        f.gate.release(sigap::debug::IoOwner::DebugHttp);
      }
      f.step(ca::https_command("start", input.first, "ambiguous"), false);
      TEST_ASSERT_TRUE(f.controller.active());
    }
  }
}
void test_https_missing_optional_setup_and_nested_same_name_remain_valid() {
  for (const auto payload : {"{}", "{\"reference_mass_mg\":1000000,\"metadata\":{\"reference_mass_mg\":0}}"}) {
    HttpsFixture f;
    f.step(ca::https_command("start", payload, "valid-optional"), false);
    TEST_ASSERT_TRUE(f.controller.active());
  }
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_actual_https_executor_denies_before_controller_and_setup_then_admits);
  RUN_TEST(test_https_full_actual_profile_workflow_and_exact_commit_replay);
  RUN_TEST(test_https_each_valid_step_denies_twice_without_effects_then_releases);
  RUN_TEST(test_https_canonical_identity_and_nonempty_payload_use_actual_network_decoder);
  RUN_TEST(test_https_replayed_start_does_not_reapply_setup_or_renew_activity);
  RUN_TEST(test_https_expected_profile_conflict_preserves_session_for_correct_commit);
  RUN_TEST(test_https_cancel_busy_has_no_effects_then_retires_the_owner_after_release);
  RUN_TEST(test_https_expiry_and_ble_disconnect_keep_https_owner_isolated);
  RUN_TEST(test_https_malformed_fields_do_not_reach_controller_or_mutate_ownership);
  RUN_TEST(test_https_start_setup_validation_precedes_controller_and_partial_ownership);
  RUN_TEST(test_https_conflicting_transport_does_not_steal_ble_owner_and_stale_ble_cleanup_is_scoped);
  RUN_TEST(test_https_setup_preflight_preserves_valid_router_boundaries);
  RUN_TEST(test_https_runtime_setup_failure_retires_only_its_new_session);
  RUN_TEST(test_https_duplicate_root_setup_keys_reject_before_controller_or_replay);
  RUN_TEST(test_https_missing_optional_setup_and_nested_same_name_remain_valid);
  UNITY_END();
}
int main() { setup(); return 0; }
