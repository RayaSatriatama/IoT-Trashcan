#include <unity.h>
#include "maintenance/runtime.h"
#include "debug/io_gate.h"
#include "config/config_store.h"
#include "provisioning/secret_store.h"
#include "admission_fixtures.h"

namespace {
using namespace sigap;
constexpr config::ValidationContext kContext{8ULL * 1024ULL * 1024ULL, 0U};
struct CountingSecretStore final : provisioning::ISecretBlobStore {
  provisioning::MemorySecretBlobStore backing{};
  mutable unsigned reads{0U};
  unsigned writes{0U};
  provisioning::SecretBlobReadStatus read(provisioning::SecretSlot slot,
      uint8_t* output, std::size_t capacity, std::size_t& size) const noexcept override {
    ++reads;
    return backing.read(slot, output, capacity, size);
  }
  bool write(provisioning::SecretSlot slot, const uint8_t* bytes,
             std::size_t size) noexcept override {
    ++writes;
    return backing.write(slot, bytes, size);
  }
  bool clear_all() noexcept override { ++writes; return backing.clear_all(); }
};
void test_secret_status_is_denied_before_persistent_reads_while_debug_owns_io() {
  config::MemoryConfigStore config_store{};
  config::ConfigManager config(config_store, kContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(config::ConfigBootResult::BootstrapDefaults), static_cast<int>(config.begin()));
  CountingSecretStore backend{};
  provisioning::PersistentSecretStore secrets(backend);
  maintenance::CommandRouter router(config, secrets);
  debug::RuntimeIoGate gate{};
  router.set_io_gate(&gate);
  maintenance::MaintenanceCommand command{};
  command.kind = maintenance::CommandKind::SecretStatus;
  maintenance::MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Accepted), static_cast<int>(router.execute(command, 1U, response)));
  TEST_ASSERT_EQUAL_UINT32(2U, backend.reads);
  TEST_ASSERT_TRUE(gate.try_acquire(debug::IoOwner::DebugHttp));
  const unsigned reads_before = backend.reads;
  response = maintenance::MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Rejected), static_cast<int>(router.execute(command, 2U, response)));
  TEST_ASSERT_TRUE(response.view().find("ERR STORAGE_BUSY") != std::string_view::npos);
  TEST_ASSERT_EQUAL_UINT32(reads_before, backend.reads);
}
void test_command_router_rejects_mutation_before_state_change_while_debug_owns_io() {
  config::MemoryConfigStore store{};
  config::ConfigManager config(store, kContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(config::ConfigBootResult::BootstrapDefaults), static_cast<int>(config.begin()));
  provisioning::MemorySecretBlobStore secret_store{};
  provisioning::PersistentSecretStore secrets(secret_store);
  maintenance::CommandRouter router(config, secrets);
  debug::RuntimeIoGate gate{};
  router.set_io_gate(&gate);
  const auto mode_before = router.mode();
  TEST_ASSERT_TRUE(gate.try_acquire(debug::IoOwner::DebugHttp));
  maintenance::MaintenanceCommand command{};
  command.kind = maintenance::CommandKind::MaintenanceEnter;
  maintenance::MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Rejected), static_cast<int>(router.execute(command, 1U, response)));
  TEST_ASSERT_TRUE(response.view().find("ERR STORAGE_BUSY") != std::string_view::npos);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(mode_before), static_cast<int>(router.mode()));
  gate.release(debug::IoOwner::DebugHttp);
  response = maintenance::MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Accepted), static_cast<int>(router.execute(command, 2U, response)));
}
}

namespace {
namespace a = admission_test;
using K = sigap::maintenance::CommandKind;

void test_config_draft_and_commit_are_unchanged_before_busy_rejection() {
  a::Fixture f;
  a::accepted(f.router, a::command(K::ConfigDraftStart));
  auto set = a::command(K::ConfigSet);
  TEST_ASSERT_TRUE(set.field.assign("sampling.mq_period_ms"));
  set.has_numeric = true; set.numeric_value = 250;
  a::accepted(f.router, set);
  a::accepted(f.router, a::command(K::ConfigValidate));
  a::accepted(f.router, a::command(K::ConfigPreview));
  const auto state = f.config.state();
  TEST_ASSERT_TRUE(f.config.has_draft());
  TEST_ASSERT_EQUAL_UINT32(250U, f.config.draft().sampling.mq_period_ms);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  set.numeric_value = 500;
  a::denied(f, set);
  a::denied(f, a::command(K::ConfigCancel));
  a::denied(f, a::command(K::ConfigCommit));
  TEST_ASSERT_TRUE(f.config.has_draft());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(state), static_cast<int>(f.config.state()));
  TEST_ASSERT_EQUAL_UINT32(250U, f.config.draft().sampling.mq_period_ms);
  const auto writes = f.config_store.calls.writes;
  const auto commits = f.audit.configs;
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  a::accepted(f.router, a::command(K::ConfigCommit));
  TEST_ASSERT_GREATER_THAN(writes, f.config_store.calls.writes);
  TEST_ASSERT_EQUAL_UINT32(commits + 1U, f.audit.configs);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigState::RebootRequired),
                        static_cast<int>(f.config.state()));
}

void test_secret_writes_have_valid_positive_busy_denial_and_one_explicit_retry() {
  for (K kind : {K::SecretSetWifi, K::SecretRotateToken, K::SecretClear}) {
    a::Fixture f;
    auto cmd = a::command(kind, "synthetic-network");
    TEST_ASSERT_TRUE(cmd.value.assign(kind == K::SecretClear ? "CONFIRM" : "synthetic-test-value"));
    const auto initial = f.secret_store.calls.writes;
    a::accepted(f.router, cmd);
    TEST_ASSERT_EQUAL_UINT32(initial + 1U, f.secret_store.calls.writes);
    TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
    a::denied(f, cmd);
    const auto held = f.secret_store.calls.writes;
    f.gate.release(sigap::debug::IoOwner::DebugHttp);
    a::accepted(f.router, cmd);
    TEST_ASSERT_EQUAL_UINT32(held + 1U, f.secret_store.calls.writes);
  }
}

void test_profile_commit_and_rollback_reject_before_storage_generation_or_audit_changes() {
  a::Fixture f;
  constexpr auto target = sigap::acquisition::ProfileTarget::Hx711Organic;
  TEST_ASSERT_TRUE(f.profiles.load(target));
  for (uint32_t version : {1U, 2U}) {
    TEST_ASSERT_TRUE(f.profiles.stage_import(target, a::profile(f, version)));
    TEST_ASSERT_TRUE(f.profiles.validate(target));
    TEST_ASSERT_TRUE(f.profiles.preview(target));
    const auto generation = f.profiles.generation();
    const auto writes = f.profile_store.calls.writes;
    const auto activated = f.audit.profiles;
    TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
    a::denied(f, a::command(K::ProfileCommit, "hx711_organic"));
    a::denied(f, a::command(K::ProfileReset, "hx711_organic"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::acquisition::ProfileManagerState::Preview),
                          static_cast<int>(f.profiles.state(target)));
    TEST_ASSERT_EQUAL_UINT32(generation, f.profiles.generation());
    f.gate.release(sigap::debug::IoOwner::DebugHttp);
    a::accepted(f.router, a::command(K::ProfileCommit, "hx711_organic"));
    TEST_ASSERT_GREATER_THAN(writes, f.profile_store.calls.writes);
    TEST_ASSERT_EQUAL_UINT32(activated + 1U, f.audit.profiles);
    sigap::acquisition::ActiveProfileSnapshot current{};
    TEST_ASSERT_TRUE(f.profiles.snapshot(target, current));
    TEST_ASSERT_EQUAL_UINT32(version, current.ref.profile_version);
  }
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  a::denied(f, a::command(K::ProfileRollback, "hx711_organic"));
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  a::accepted(f.router, a::command(K::ProfileRollback, "hx711_organic"));
  sigap::acquisition::ActiveProfileSnapshot previous{};
  TEST_ASSERT_TRUE(f.profiles.snapshot(target, previous));
  TEST_ASSERT_EQUAL_UINT32(1U, previous.ref.profile_version);
  TEST_ASSERT_EQUAL_UINT32(1U, f.audit.rollbacks);
}

void test_storage_probe_and_audit_context_have_no_calls_during_repeated_denial() {
  a::Fixture f;
  const auto cmd = a::command(K::StorageSdTest);
  a::accepted(f.router, cmd);
  TEST_ASSERT_EQUAL_UINT32(1U, f.probe.calls);
  TEST_ASSERT_GREATER_THAN(0U, f.audit.contexts);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  a::denied(f, cmd);
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  a::accepted(f.router, cmd);
  TEST_ASSERT_EQUAL_UINT32(2U, f.probe.calls);
}

void test_direct_model_and_command_router_deny_commit_and_rollback_before_io() {
  a::Fixture f;
  TEST_ASSERT_TRUE(f.model_lifecycle.load(a::model_key));
  for (uint32_t version : {1U, 2U}) {
    TEST_ASSERT_TRUE(f.model_lifecycle.stage(a::model_key, a::model(f, version)));
    TEST_ASSERT_TRUE(f.model_lifecycle.validate(a::model_key));
    TEST_ASSERT_TRUE(f.model_lifecycle.preview(a::model_key));
    const auto cmd = a::command(K::ModelCommit, "safety_b3");
    TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
    a::denied(f, cmd);
    const auto before = a::snapshot(f);
    sigap::maintenance::MaintenanceResponse response{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::maintenance::CommandStatus::Rejected),
                          static_cast<int>(f.model_router.execute_model(cmd, 999U, response)));
    TEST_ASSERT_EQUAL_STRING("ERR STORAGE_BUSY\n", response.c_str());
    a::unchanged(f, before);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::models::ModelLifecycleState::Preview),
                          static_cast<int>(f.model_lifecycle.state(a::model_key)));
    f.gate.release(sigap::debug::IoOwner::DebugHttp);
    const auto writes = f.model_store.calls.writes;
    a::accepted(f.router, cmd);
    TEST_ASSERT_GREATER_THAN(writes, f.model_store.calls.writes);
  }
  const auto rollback = a::command(K::ModelRollback, "safety_b3");
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  a::denied(f, rollback);
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  a::accepted(f.model_router, rollback);
  sigap::models::ModelProfileSnapshot previous{};
  TEST_ASSERT_TRUE(f.model_lifecycle.snapshot(a::model_key, previous));
  TEST_ASSERT_EQUAL_UINT32(1U, previous.ref.profile_version);
}

void import_model_draft(a::Fixture& f) {
  TEST_ASSERT_TRUE(f.model_lifecycle.load(a::model_key));
  const auto encoded = a::model(f, 1U);
  auto begin = a::command(K::ModelImportBegin, "safety_b3");
  begin.has_numeric = true; begin.numeric_value = static_cast<int64_t>(encoded.size);
  const auto digest = encoded.hash();
  TEST_ASSERT_TRUE(begin.value.assign(a::hex(digest.data(), digest.size())));
  a::accepted(f.model_router, begin, 1U);
  for (std::size_t offset = 0U; offset < encoded.size; offset += 48U) {
    auto chunk = a::command(K::ModelImportChunk, "safety_b3");
    chunk.has_numeric = true; chunk.numeric_value = static_cast<int64_t>(offset);
    TEST_ASSERT_TRUE(chunk.value.assign(a::hex(encoded.bytes.data() + offset,
                                             std::min<std::size_t>(48U, encoded.size - offset))));
    a::accepted(f.model_router, chunk, 2U);
  }
  a::accepted(f.model_router, a::command(K::ModelImportFinish, "safety_b3"), 3U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::models::ModelLifecycleState::Draft),
                        static_cast<int>(f.model_lifecycle.state(a::model_key)));
}

void test_model_owned_draft_timeout_cancels_in_ram_while_debug_keeps_the_gate() {
  a::Fixture f;
  import_model_draft(f);
  const auto before = a::snapshot(f);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  f.router.poll(sigap::maintenance::ModelMaintenanceRouter::kTransferTimeoutUs + 4U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::models::ModelLifecycleState::Unloaded),
                        static_cast<int>(f.model_lifecycle.state(a::model_key)));
  a::unchanged(f, before);
  TEST_ASSERT_TRUE(f.gate.active(sigap::debug::IoOwner::DebugHttp));
}

void test_ram_model_cancel_is_available_directly_and_through_command_router() {
  a::Fixture f;
  import_model_draft(f);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto before = a::snapshot(f);
  a::accepted(f.model_router, a::command(K::ModelCancel, "safety_b3"));
  a::unchanged(f, before);
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  import_model_draft(f);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto second = a::snapshot(f);
  a::accepted(f.router, a::command(K::ModelCancel, "safety_b3"));
  // Command context updates are RAM-only; persistent calls still must not change.
  a::unchanged(second.config, f.config_store.calls);
  a::unchanged(second.secret, f.secret_store.calls);
  a::unchanged(second.profile, f.profile_store.calls);
  a::unchanged(second.model, f.model_store.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::models::ModelLifecycleState::Unloaded),
                        static_cast<int>(f.model_lifecycle.state(a::model_key)));
}

void test_calibration_internal_admission_rejects_before_owning_or_applying_setup() {
  a::Fixture f;
  constexpr auto target = sigap::calibration::CalibrationTarget::Hx711Organic;
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto before = a::snapshot(f);
  TEST_ASSERT_FALSE(f.router.start(target, 1U));
  sigap::calibration::CalibrationStatusSnapshot status{};
  TEST_ASSERT_TRUE(f.router.status(status));
  TEST_ASSERT_FALSE(status.active);
  a::unchanged(f, before);
  f.gate.release(sigap::debug::IoOwner::DebugHttp);
  TEST_ASSERT_TRUE(f.router.start(target, 2U));
  TEST_ASSERT_TRUE(f.router.status(status));
  TEST_ASSERT_TRUE(status.active);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  auto criteria = a::command(K::CalibrationCriteria);
  TEST_ASSERT_TRUE(criteria.field.assign("samples"));
  criteria.has_numeric = true; criteria.numeric_value = 10;
  a::denied(f, criteria);
  f.router.poll(sigap::calibration::CalibrationSession::kSessionTimeoutUs + 3U);
  TEST_ASSERT_TRUE(f.router.status(status));
  TEST_ASSERT_FALSE(status.active);
  TEST_ASSERT_TRUE(f.gate.active(sigap::debug::IoOwner::DebugHttp));
}

void test_ram_calibration_cancel_remains_available_without_persistent_calls() {
  a::Fixture f;
  TEST_ASSERT_TRUE(f.router.start(sigap::calibration::CalibrationTarget::Hx711Organic, 1U));
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto before = a::snapshot(f);
  a::accepted(f.router, a::command(K::CalibrationCancel));
  a::unchanged(before.config, f.config_store.calls);
  a::unchanged(before.secret, f.secret_store.calls);
  a::unchanged(before.profile, f.profile_store.calls);
  a::unchanged(before.model, f.model_store.calls);
  sigap::calibration::CalibrationStatusSnapshot status{};
  TEST_ASSERT_TRUE(f.router.status(status));
  TEST_ASSERT_FALSE(status.active);
}

void test_ram_statuses_do_not_read_any_persistent_backend_while_debug_is_active() {
  a::Fixture f;
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto before = a::snapshot(f);
  a::accepted(f.router, a::command(K::Status));
  a::accepted(f.router, a::command(K::SensorStatus, "hx711_organic"));
  a::accepted(f.router, a::command(K::ModelStatus, "safety_b3"));
  a::unchanged(before.config, f.config_store.calls);
  a::unchanged(before.secret, f.secret_store.calls);
  a::unchanged(before.profile, f.profile_store.calls);
  a::unchanged(before.model, f.model_store.calls);
  TEST_ASSERT_EQUAL_UINT32(0U, f.probe.calls);
}

void test_profile_owned_transfer_expiry_only_clears_ram_while_debug_is_active() {
  a::Fixture f;
  constexpr auto target = sigap::acquisition::ProfileTarget::Hx711Organic;
  TEST_ASSERT_TRUE(f.profiles.load(target));
  const auto encoded = a::profile(f, 1U);
  auto begin = a::command(K::ProfileImportBegin, "hx711_organic");
  begin.has_numeric = true; begin.numeric_value = static_cast<int64_t>(encoded.size);
  const auto digest = encoded.hash();
  TEST_ASSERT_TRUE(begin.value.assign(a::hex(digest.data(), digest.size())));
  a::accepted(f.router, begin, 1U);
  for (std::size_t offset = 0U; offset < encoded.size; offset += 48U) {
    auto chunk = a::command(K::ProfileImportChunk);
    chunk.has_numeric = true; chunk.numeric_value = static_cast<int64_t>(offset);
    TEST_ASSERT_TRUE(chunk.value.assign(a::hex(encoded.bytes.data() + offset,
                                             std::min<std::size_t>(48U, encoded.size - offset))));
    a::accepted(f.router, chunk, 2U);
  }
  a::accepted(f.router, a::command(K::ProfileImportFinish, "hx711_organic"), 3U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::acquisition::ProfileManagerState::Draft),
                        static_cast<int>(f.profiles.state(target)));
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  const auto before = a::snapshot(f);
  f.router.poll(sigap::maintenance::CommandRouter::kTransferTimeoutUs + 4U);
  a::unchanged(f, before);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::acquisition::ProfileManagerState::Unloaded),
                        static_cast<int>(f.profiles.state(target)));
  TEST_ASSERT_TRUE(f.gate.active(sigap::debug::IoOwner::DebugHttp));
}

void test_optional_binding_preserves_legacy_behavior_but_never_claims_unbound_enforcement() {
  a::Fixture f;
  f.router.set_io_gate(nullptr);
  TEST_ASSERT_TRUE(f.gate.try_acquire(sigap::debug::IoOwner::DebugHttp));
  auto cmd = a::command(K::SecretRotateToken);
  TEST_ASSERT_TRUE(cmd.value.assign("synthetic-test-token"));
  a::accepted(f.router, cmd);
  TEST_ASSERT_EQUAL_UINT32(1U, f.secret_store.calls.writes);
  f.router.set_io_gate(&f.gate);
  a::denied(f, cmd);
}
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_secret_status_is_denied_before_persistent_reads_while_debug_owns_io);
  RUN_TEST(test_command_router_rejects_mutation_before_state_change_while_debug_owns_io);
  RUN_TEST(test_config_draft_and_commit_are_unchanged_before_busy_rejection);
  RUN_TEST(test_secret_writes_have_valid_positive_busy_denial_and_one_explicit_retry);
  RUN_TEST(test_profile_commit_and_rollback_reject_before_storage_generation_or_audit_changes);
  RUN_TEST(test_storage_probe_and_audit_context_have_no_calls_during_repeated_denial);
  RUN_TEST(test_direct_model_and_command_router_deny_commit_and_rollback_before_io);
  RUN_TEST(test_model_owned_draft_timeout_cancels_in_ram_while_debug_keeps_the_gate);
  RUN_TEST(test_ram_model_cancel_is_available_directly_and_through_command_router);
  RUN_TEST(test_calibration_internal_admission_rejects_before_owning_or_applying_setup);
  RUN_TEST(test_ram_calibration_cancel_remains_available_without_persistent_calls);
  RUN_TEST(test_ram_statuses_do_not_read_any_persistent_backend_while_debug_is_active);
  RUN_TEST(test_profile_owned_transfer_expiry_only_clears_ram_while_debug_is_active);
  RUN_TEST(test_optional_binding_preserves_legacy_behavior_but_never_claims_unbound_enforcement);
  return UNITY_END();
}
