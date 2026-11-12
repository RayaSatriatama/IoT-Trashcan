#include <string_view>

#include <unity.h>

#include "maintenance/ble_calibration_backend.h"
#include "maintenance/runtime.h"
#include "config/config_store.h"
#include "config/config_manager.h"
#include "config/config_validator.h"
#include "provisioning/secret_store.h"
#include "../test_calibration_admission/calibration_fixtures.h"
#include "debug/io_gate.h"

namespace {

using namespace sigap::maintenance;

constexpr sigap::config::ValidationContext kContext{8ULL * 1024ULL * 1024ULL,
                                                     0U};

class Clock final : public ICalibrationClock {
 public:
  uint64_t now_us() const noexcept override { return now; }
  uint64_t now{100U};
};

class Authorizer final : public sigap::calibration::ICalibrationTransportAuthorizer {
 public:
  bool authorize(const sigap::calibration::CalibrationCommandEnvelope& command) const noexcept override {
    return command.transport == sigap::calibration::CalibrationTransport::Ble;
  }
};

class ReplayStore final : public sigap::calibration::ICalibrationTransportReplayStore {
 public:
  bool prepare(const sigap::calibration::CalibrationCommandEnvelope&) noexcept override {
    ++prepares;
    return true;
  }
  bool load(uint64_t request_id,
            sigap::calibration::CalibrationCommandEnvelope&,
            sigap::calibration::CalibrationResultEnvelope&) const noexcept override {
    ++loads;
    last_request_id = request_id;
    return false;
  }
  bool load_prepared(uint64_t,
                     sigap::calibration::CalibrationCommandEnvelope&) const noexcept override {
    ++prepared_loads;
    return false;
  }
  bool save(const sigap::calibration::CalibrationCommandEnvelope&,
            const sigap::calibration::CalibrationResultEnvelope&) noexcept override {
    ++saves;
    return true;
  }
  unsigned prepares{0U};
  mutable unsigned loads{0U};
  mutable unsigned prepared_loads{0U};
  unsigned saves{0U};
  mutable uint64_t last_request_id{0U};
};

void test_ble_adapter_uses_authoritative_parser_and_router() {
  sigap::provisioning::BleCommand parsed{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::provisioning::BleParseStatus::Accepted),
      static_cast<int>(sigap::provisioning::BleCommandParser::parse(
          "CAL STATUS", parsed)));

  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config(config_store, kContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_store{};
  sigap::provisioning::PersistentSecretStore secrets(secret_store);
  CommandRouter router(config, secrets);
  Clock clock{};
  CommandRouterBleCalibrationBackend backend(router, clock);
  char response[512]{};
  std::size_t size{0U};

  TEST_ASSERT_TRUE(backend.execute("CAL STATUS", response, sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "calibration_active=false") != std::string_view::npos);

  size = 0U;
  TEST_ASSERT_FALSE(backend.execute("CAL UNKNOWN", response, sizeof(response), size));
  size = 0U;
  TEST_ASSERT_TRUE(backend.execute("CAL START hx711_organic", response,
                                   sizeof(response), size));
  TEST_ASSERT_TRUE(backend.execute(
      "CAL CRITERIA hx711_organic samples=2 max_span_counts=10",
      response, sizeof(response), size));
  TEST_ASSERT_TRUE(backend.execute("CAL CAPTURE hx711_organic tare", response,
                                   sizeof(response), size) == false);
  // A BLE-owned session is the only session a BLE disconnect may cancel.
  backend.disconnect();
  size = 0U;
  TEST_ASSERT_TRUE(backend.execute("CAL STATUS", response, sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "calibration_active=false") != std::string_view::npos);
}

void test_ble_calibration_envelope_and_typed_capabilities() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config(config_store, kContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_store{};
  sigap::provisioning::PersistentSecretStore secrets(secret_store);
  CommandRouter router(config, secrets);
  Clock clock{};
  CommandRouterBleCalibrationBackend backend(router, clock);
  char response[512]{};
  std::size_t size{0U};

  TEST_ASSERT_TRUE(backend.execute("CAL CAPABILITIES ds18b20_b3", response,
                                  sizeof(response), size));
  const std::string_view capabilities(response, size);
  TEST_ASSERT_TRUE(capabilities.find("CAL1|op=CAPABILITIES|status=ACCEPTED") !=
                  std::string_view::npos);
  TEST_ASSERT_TRUE(capabilities.find("reference_temperature_centi_c") !=
                  std::string_view::npos);

  size = 0U;
  TEST_ASSERT_FALSE(backend.execute("CAL HISTORY ds18b20_b3", response,
                                   sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "status=ERROR") != std::string_view::npos);
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "UNSUPPORTED") != std::string_view::npos);

  sigap::provisioning::BleCommand parsed{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::provisioning::BleParseStatus::Accepted),
      static_cast<int>(sigap::provisioning::BleCommandParser::parse(
          "CAL ACTIVE_PROFILE ds18b20_b3", parsed)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::provisioning::BleCommandKind::CalibrationActiveProfile),
      static_cast<int>(parsed.kind));
}

void test_ble_mutations_use_shared_controller_owner() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config(config_store, kContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_store{};
  sigap::provisioning::PersistentSecretStore secrets(secret_store);
  CommandRouter router(config, secrets);
  Clock clock{};
  Authorizer authorizer{};
  sigap::calibration::CalibrationTransportController controller(
      router, 7U, &authorizer);
  CommandRouterBleCalibrationBackend backend(router, clock, &controller);
  router.set_calibration_transport_controller(&controller);
  char response[512]{};
  std::size_t size{0U};

  TEST_ASSERT_TRUE(backend.execute("CAL START hx711_organic", response,
                                  sizeof(response), size));
  TEST_ASSERT_TRUE(controller.active());
  TEST_ASSERT_TRUE(std::string_view(response, size).find("status=ACCEPTED") !=
                  std::string_view::npos);
  MaintenanceParser parser{};
  ParseEvent event{};
  for (const char byte : std::string_view("calibration criteria samples 2\n")) {
    (void)parser.consume(byte, event);
  }
  MaintenanceResponse usb_response{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CommandStatus::Rejected),
      static_cast<int>(router.execute(event.command, clock.now, usb_response)));
  size = 0U;
  TEST_ASSERT_TRUE(backend.execute(
      "CAL CRITERIA hx711_organic samples=2 max_span_counts=10", response,
      sizeof(response), size));
  TEST_ASSERT_TRUE(controller.active());
  backend.disconnect();
  TEST_ASSERT_FALSE(controller.active());

  size = 0U;
  TEST_ASSERT_TRUE(backend.execute(
      "CAL START hx711_organic reference_mass_mg=2000", response,
      sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "sensor=hx711_organic") != std::string_view::npos);
  size = 0U;
  TEST_ASSERT_FALSE(backend.execute("CAL COMMIT hx711_organic", response,
                                   sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "status=ERROR") != std::string_view::npos);
  backend.disconnect();
  TEST_ASSERT_FALSE(controller.active());

  size = 0U;
  TEST_ASSERT_TRUE(backend.execute(
      "CAL START sht31_44 address=68", response, sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "sensor=sht31_44") != std::string_view::npos);
  backend.disconnect();
}

}  // namespace

void test_debug_http_gate_precedes_ble_controller_and_replay() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config(config_store, kContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
                        static_cast<int>(config.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_store{};
  sigap::provisioning::PersistentSecretStore secrets(secret_store);
  CommandRouter router(config, secrets);
  Clock clock{};
  Authorizer authorizer{};
  ReplayStore replay{};
  sigap::calibration::CalibrationTransportController controller(router, 7U, &authorizer, &replay);
  router.set_calibration_transport_controller(&controller);
  CommandRouterBleCalibrationBackend backend(router, clock, &controller);
  sigap::debug::RuntimeIoGate gate{};
  backend.set_io_gate(&gate);
  TEST_ASSERT_TRUE(gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  char response[512]{};
  std::size_t size{0U};
  TEST_ASSERT_FALSE(backend.execute("CAL START hx711_organic reference_mass_mg=2000",
                                   response, sizeof(response), size));
  TEST_ASSERT_FALSE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(0U, replay.loads);
  TEST_ASSERT_EQUAL_UINT32(0U, replay.prepared_loads);
  TEST_ASSERT_EQUAL_UINT32(0U, replay.prepares);
  TEST_ASSERT_EQUAL_UINT32(0U, replay.saves);
  gate.release(sigap::debug::IoOwner::DebugHttp);
  TEST_ASSERT_TRUE(backend.execute("CAL START hx711_organic reference_mass_mg=2000",
                                  response, sizeof(response), size));
  TEST_ASSERT_TRUE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(1U, replay.loads);
  TEST_ASSERT_EQUAL_UINT64(1U, replay.last_request_id);
  TEST_ASSERT_TRUE(gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  size = 0U;
  TEST_ASSERT_FALSE(backend.execute("CAL CANCEL hx711_organic", response,
                                   sizeof(response), size));
  TEST_ASSERT_TRUE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(1U, replay.loads);
  TEST_ASSERT_EQUAL_UINT32(0U, replay.saves);
  gate.release(sigap::debug::IoOwner::DebugHttp);
  TEST_ASSERT_TRUE(backend.execute("CAL CANCEL hx711_organic", response,
                                  sizeof(response), size));
  TEST_ASSERT_FALSE(controller.active());
  TEST_ASSERT_EQUAL_UINT32(2U, replay.loads);
  TEST_ASSERT_EQUAL_UINT64(2U, replay.last_request_id);
}

namespace ca = calibration_admission_test;

struct OwnerEvents final : sigap::debug::IEventSink {
  bool emit(const sigap::debug::EventInput& event) noexcept override {
    if (event.event_code == 51U) {
      last = event;
      ++count;
    }
    return true;
  }
  sigap::debug::EventInput last{};
  unsigned count{};
};

void test_ble_disconnect_emits_owner_free_event_at_observed_clock_time() {
  ca::Fixture f;
  OwnerEvents events;
  f.controller.set_debug_sink(&events);
  ca::ble_step(f, "CAL START hx711_organic", false);
  TEST_ASSERT_TRUE(f.controller.active());
  TEST_ASSERT_EQUAL_UINT32(1U, events.count);
  TEST_ASSERT_EQUAL_INT32(2, events.last.arg0);

  f.clock.now = 987654U;
  f.ble.disconnect();
  TEST_ASSERT_FALSE(f.controller.active());
  TEST_ASSERT_EQUAL_UINT32(2U, events.count);
  TEST_ASSERT_EQUAL_UINT64(987654U, events.last.monotonic_us);
  TEST_ASSERT_EQUAL_INT32(0, events.last.arg0);
  TEST_ASSERT_EQUAL_INT32(0, events.last.arg1);
}

void full_ble_workflow(ca::Fixture& f, bool held) {
  ca::ble_step(f, "CAL START hx711_organic reference_mass_mg=1000000", held);
  ca::ble_step(f, "CAL CRITERIA hx711_organic samples=1 max_span_counts=10", held);
  ca::ble_step(f, "CAL CAPTURE hx711_organic tare", held);
  f.reference();
  ca::ble_step(f, "CAL CAPTURE hx711_organic reference", held);
  ca::ble_step(f, "CAL VALIDATE hx711_organic", held);
  ca::ble_step(f, "CAL PREVIEW hx711_organic", held);
  const auto proposed = f.proposal();
  ca::ble_step(f, ca::ble_commit(proposed), held);
  f.committed(proposed);
  const auto writes = f.profile_store.calls.writes;
  TEST_ASSERT_FALSE(ca::ble(f, ca::ble_commit(proposed)));
  TEST_ASSERT_EQUAL_UINT32(writes, f.profile_store.calls.writes);
  TEST_ASSERT_EQUAL_UINT32(1U, f.audit.profiles);
}
void test_ble_full_actual_profile_workflow_and_duplicate_commit() {
  ca::Fixture f;
  full_ble_workflow(f, false);
}
void test_ble_each_valid_workflow_step_denies_twice_without_effects_then_releases() {
  ca::Fixture f;
  full_ble_workflow(f, true);
}
void test_ble_conflicting_commit_preserves_owner_for_corrected_request() {
  ca::Fixture f;
  ca::ble_step(f, "CAL START hx711_organic reference_mass_mg=1000000", false);
  ca::ble_step(f, "CAL CRITERIA hx711_organic samples=1 max_span_counts=10", false);
  ca::ble_step(f, "CAL CAPTURE hx711_organic tare", false);
  f.reference();
  ca::ble_step(f, "CAL CAPTURE hx711_organic reference", false);
  ca::ble_step(f, "CAL VALIDATE hx711_organic", false);
  ca::ble_step(f, "CAL PREVIEW hx711_organic", false);
  auto wrong = f.proposal();
  const auto correct = wrong;
  ++wrong.version;
  const auto writes = f.profile_store.calls.writes;
  TEST_ASSERT_FALSE(ca::ble(f, ca::ble_commit(wrong)));
  TEST_ASSERT_TRUE(f.controller.active());
  TEST_ASSERT_EQUAL_UINT32(writes, f.profile_store.calls.writes);
  ca::ble_step(f, ca::ble_commit(correct), false);
  f.committed(correct);
}
void test_ble_cancel_busy_does_not_retire_owner_or_consume_request_identity() {
  ca::Fixture f;
  ca::ble_step(f, "CAL START hx711_organic reference_mass_mg=1000000", false);
  ca::ble_step(f, "CAL CANCEL hx711_organic", true);
  TEST_ASSERT_FALSE(f.controller.active());
  TEST_ASSERT_EQUAL_UINT32(0U, f.profile_store.calls.writes);
}
void test_ble_invalid_payloads_do_not_touch_workflow_or_allocate_identity() {
  for (const bool held : {false, true}) {
    ca::Fixture f;
    if (held) TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
    const auto before = f.snapshot();
    for (const auto text : {"WRONG", "CAL UNKNOWN hx711_organic", "CAL START no-sensor",
        "CAL START hx711_organic reference_mass_mg=broken",
        "CAL CRITERIA hx711_organic samples=bad max_span_counts=10",
        "CAL COMMIT hx711_organic expected_profile_version=0 expected_profile_hash=sha256:bad"}) {
      TEST_ASSERT_FALSE(ca::ble(f, text));
      ca::same(before, f.snapshot());
    }
    if (held) f.gate.release(sigap::debug::IoOwner::DebugHttp);
    ca::ble_step(f, "CAL START hx711_organic", false);
    TEST_ASSERT_EQUAL_UINT64(1U, f.authorizer.last_request);
  }
}
void test_ble_expiry_and_disconnect_cleanup_progress_without_persistent_io() {
  ca::Fixture f;
  ca::ble_step(f, "CAL START hx711_organic", false);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto storage = f.storage();
  f.clock.now += sigap::calibration::CalibrationSession::kSessionTimeoutUs + 1U;
  TEST_ASSERT_FALSE(ca::ble(f, "CAL CANCEL hx711_organic"));
  TEST_ASSERT_TRUE(f.controller.active());
  f.controller.poll(f.clock.now);
  TEST_ASSERT_FALSE(f.controller.active());
  ca::same(storage, f.storage());
  f.ble.disconnect();
  ca::same(storage, f.storage());
  TEST_ASSERT_TRUE(f.gate.active(sigap::debug::IoOwner::DebugHttp));
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  ca::ble_step(f, "CAL START hx711_organic", false);
  TEST_ASSERT_TRUE(f.controller.active());
}

void test_ble_read_only_operations_preserve_existing_policy_and_do_no_persistent_io() {
  const std::array<const char*, 4> commands{{"CAL CAPABILITIES hx711_organic", "CAL STATUS",
      "CAL EVIDENCE hx711_organic", "CAL ACTIVE_PROFILE hx711_organic"}};
  for (const bool held : {false, true}) {
    ca::Fixture f;
    if (held) TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
    const auto before = f.storage();
    for (std::size_t i = 0U; i < commands.size(); ++i) {
      const bool expected = !held || i == 0U || i == 2U;
      TEST_ASSERT_EQUAL_INT(expected, ca::ble(f, commands[i]));
      ca::same(before, f.storage());
      TEST_ASSERT_FALSE(f.controller.active());
      TEST_ASSERT_EQUAL_UINT32(0U, f.authorizer.calls);
    }
  }
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ble_adapter_uses_authoritative_parser_and_router);
  RUN_TEST(test_ble_calibration_envelope_and_typed_capabilities);
  RUN_TEST(test_ble_mutations_use_shared_controller_owner);
  RUN_TEST(test_debug_http_gate_precedes_ble_controller_and_replay);
  RUN_TEST(test_ble_full_actual_profile_workflow_and_duplicate_commit);
  RUN_TEST(test_ble_each_valid_workflow_step_denies_twice_without_effects_then_releases);
  RUN_TEST(test_ble_conflicting_commit_preserves_owner_for_corrected_request);
  RUN_TEST(test_ble_cancel_busy_does_not_retire_owner_or_consume_request_identity);
  RUN_TEST(test_ble_invalid_payloads_do_not_touch_workflow_or_allocate_identity);
  RUN_TEST(test_ble_expiry_and_disconnect_cleanup_progress_without_persistent_io);
  RUN_TEST(test_ble_read_only_operations_preserve_existing_policy_and_do_no_persistent_io);
  RUN_TEST(test_ble_disconnect_emits_owner_free_event_at_observed_clock_time);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
