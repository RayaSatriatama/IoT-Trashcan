#pragma once

#include <algorithm>
#include <string>
#include <unity.h>
#include "maintenance/runtime.h"
#include "maintenance/model_runtime.h"
#include "config/config_store.h"
#include "acquisition/profile_store.h"
#include "models/model_lifecycle.h"
#include "debug/io_gate.h"

namespace admission_test {
using namespace sigap;

struct Counts {
  unsigned reads{0U};
  unsigned writes{0U};
};
inline void unchanged(Counts before, Counts after) {
  TEST_ASSERT_EQUAL_UINT32(before.reads, after.reads);
  TEST_ASSERT_EQUAL_UINT32(before.writes, after.writes);
}

struct ConfigStore final : config::IConfigSlotStore {
  config::MemoryConfigStore backing{};
  mutable Counts calls{};
  config::StoreReadStatus read_activation_metadata(config::ActivationMetadata& out) const noexcept override {
    ++calls.reads; return backing.read_activation_metadata(out);
  }
  config::StoreReadStatus read_slot(config::ConfigSlot slot, config::EncodedConfig& out) const noexcept override {
    ++calls.reads; return backing.read_slot(slot, out);
  }
  bool write_slot(config::ConfigSlot slot, const config::EncodedConfig& value) noexcept override {
    ++calls.writes; return backing.write_slot(slot, value);
  }
  bool write_activation_metadata(const config::ActivationMetadata& value) noexcept override {
    ++calls.writes; return backing.write_activation_metadata(value);
  }
  config::ProvisioningStatus provisioning_status() const noexcept override {
    ++calls.reads; return backing.provisioning_status();
  }
  bool mark_provisioned() noexcept override { ++calls.writes; return backing.mark_provisioned(); }
};

struct Secrets final : provisioning::ISecretBlobStore {
  provisioning::MemorySecretBlobStore backing{};
  mutable Counts calls{};
  provisioning::SecretBlobReadStatus read(provisioning::SecretSlot slot, uint8_t* out,
      std::size_t capacity, std::size_t& size) const noexcept override {
    ++calls.reads; return backing.read(slot, out, capacity, size);
  }
  bool write(provisioning::SecretSlot slot, const uint8_t* bytes, std::size_t size) noexcept override {
    ++calls.writes; return backing.write(slot, bytes, size);
  }
  bool clear_all() noexcept override { ++calls.writes; return backing.clear_all(); }
};

struct Profiles final : acquisition::IProfileSlotStore {
  acquisition::MemoryProfileStore backing{};
  mutable Counts calls{};
  acquisition::ProfileReadStatus read_activation(acquisition::ProfileTarget target,
      acquisition::ProfileActivationMetadata& out) const noexcept override {
    ++calls.reads; return backing.read_activation(target, out);
  }
  acquisition::ProfileReadStatus read_slot(acquisition::ProfileTarget target,
      acquisition::ProfileSlot slot, acquisition::EncodedProfile& out) const noexcept override {
    ++calls.reads; return backing.read_slot(target, slot, out);
  }
  bool write_slot(acquisition::ProfileTarget target, acquisition::ProfileSlot slot,
      const acquisition::EncodedProfile& value) noexcept override {
    ++calls.writes; return backing.write_slot(target, slot, value);
  }
  bool write_activation(acquisition::ProfileTarget target,
      const acquisition::ProfileActivationMetadata& value) noexcept override {
    ++calls.writes; return backing.write_activation(target, value);
  }
};

struct Models final : models::IModelSlotStore {
  models::MemoryModelSlotStore backing{};
  mutable Counts calls{};
  models::ModelLoadStatus read_activation(models::ModelProfileKey key,
      models::ModelActivationMetadata& out) const noexcept override {
    ++calls.reads; return backing.read_activation(key, out);
  }
  models::ModelLoadStatus read_slot(models::ModelProfileKey key, models::ModelSlot slot,
      models::EncodedModelProfile& out) const noexcept override {
    ++calls.reads; return backing.read_slot(key, slot, out);
  }
  bool write_slot(models::ModelProfileKey key, models::ModelSlot slot,
      const models::EncodedModelProfile& value) noexcept override {
    ++calls.writes; return backing.write_slot(key, slot, value);
  }
  bool write_activation(models::ModelProfileKey key,
      const models::ModelActivationMetadata& value) noexcept override {
    ++calls.writes; return backing.write_activation(key, value);
  }
};

struct Audit final : acquisition::IProfileAuditSink {
  unsigned contexts{0U}, profiles{0U}, configs{0U}, rollbacks{0U};
  uint64_t last_context{0U};
  void set_context(uint64_t now, const acquisition::ConfigRef&) noexcept override {
    ++contexts; last_context = now;
  }
  void profile_activated(const acquisition::ProfileRef&, const acquisition::ProfileRef&) noexcept override { ++profiles; }
  void profile_rolled_back(const acquisition::ProfileRef&, const acquisition::ProfileRef&) noexcept override { ++rollbacks; }
  void config_committed(const acquisition::ConfigRef&) noexcept override { ++configs; }
};
struct Probe final : maintenance::IStorageTestProbe {
  unsigned calls{0U};
  maintenance::CommandStatus run_sd_probe(maintenance::MaintenanceResponse& response) noexcept override {
    ++calls; (void)response.append("HOST_PROBE\n"); return maintenance::CommandStatus::Accepted;
  }
};

struct Fixture {
  static constexpr config::ValidationContext context{8ULL * 1024ULL * 1024ULL, 0U};
  ConfigStore config_store{};
  config::ConfigManager config{config_store, context};
  Secrets secret_store{};
  provisioning::PersistentSecretStore secrets{secret_store};
  Audit audit{};
  Profiles profile_store{};
  acquisition::ProfileCodec profile_codec{};
  acquisition::ProfilePersistence profile_persistence{profile_store};
  acquisition::ProfileManager profiles{profile_persistence, profile_codec, &audit};
  Models model_store{};
  models::ModelProfileCodec model_codec{};
  models::ModelProfilePersistence model_persistence{model_store, model_codec};
  models::ModelProfileLifecycle model_lifecycle{model_persistence, model_codec};
  maintenance::ModelMaintenanceRouter model_router{model_lifecycle, model_codec};
  Probe probe{};
  maintenance::CommandRouter router{config, secrets, &profiles, nullptr, nullptr, &audit,
                                    nullptr, &model_router, &probe};
  debug::RuntimeIoGate gate{};
  Fixture() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::ConfigBootResult::BootstrapDefaults), static_cast<int>(config.begin()));
    router.set_io_gate(&gate);
    model_router.set_io_gate(&gate);
  }
};

inline maintenance::MaintenanceCommand command(maintenance::CommandKind kind, const char* target = "") {
  maintenance::MaintenanceCommand result{};
  result.kind = kind;
  TEST_ASSERT_TRUE(result.target.assign(target));
  return result;
}
inline void accepted(maintenance::CommandRouter& router, const maintenance::MaintenanceCommand& cmd,
                     uint64_t now = 100U) {
  maintenance::MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Accepted), static_cast<int>(router.execute(cmd, now, response)));
}
inline void accepted(maintenance::ModelMaintenanceRouter& router, const maintenance::MaintenanceCommand& cmd,
                     uint64_t now = 100U) {
  maintenance::MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Accepted), static_cast<int>(router.execute_model(cmd, now, response)));
}
struct Snapshot {
  Counts config, secret, profile, model;
  unsigned contexts, activated, committed, rolled_back, probes;
  uint64_t last_context;
};
inline Snapshot snapshot(const Fixture& f) {
  return {f.config_store.calls, f.secret_store.calls, f.profile_store.calls, f.model_store.calls,
          f.audit.contexts, f.audit.profiles, f.audit.configs, f.audit.rollbacks, f.probe.calls, f.audit.last_context};
}
inline void unchanged(const Fixture& f, const Snapshot& before) {
  unchanged(before.config, f.config_store.calls);
  unchanged(before.secret, f.secret_store.calls);
  unchanged(before.profile, f.profile_store.calls);
  unchanged(before.model, f.model_store.calls);
  TEST_ASSERT_EQUAL_UINT32(before.contexts, f.audit.contexts);
  TEST_ASSERT_EQUAL_UINT32(before.activated, f.audit.profiles);
  TEST_ASSERT_EQUAL_UINT32(before.committed, f.audit.configs);
  TEST_ASSERT_EQUAL_UINT32(before.rolled_back, f.audit.rollbacks);
  TEST_ASSERT_EQUAL_UINT32(before.probes, f.probe.calls);
  TEST_ASSERT_EQUAL_UINT64(before.last_context, f.audit.last_context);
}
inline void denied(Fixture& f, const maintenance::MaintenanceCommand& cmd) {
  TEST_ASSERT_TRUE(f.gate.active(debug::IoOwner::DebugHttp));
  const auto before = snapshot(f);
  for (unsigned i = 0U; i < 2U; ++i) {
    maintenance::MaintenanceResponse response{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(maintenance::CommandStatus::Rejected), static_cast<int>(f.router.execute(cmd, 999U, response)));
    TEST_ASSERT_EQUAL_STRING("ERR STORAGE_BUSY\n", response.c_str());
    unchanged(f, before);
    TEST_ASSERT_TRUE(f.gate.active(debug::IoOwner::DebugHttp));
  }
}
inline acquisition::EncodedProfile profile(Fixture& f, uint32_t version) {
  acquisition::Hx711Profile value{};
  value.manifest.schema_version = 1U;
  value.manifest.profile_version = version;
  value.manifest.target_id = static_cast<uint32_t>(acquisition::ProfileTarget::Hx711Organic);
  value.manifest.validity = acquisition::ProfileValidity::Valid;
  value.tare_offset_counts = 1000;
  value.counts_per_kg = 500.0F;
  acquisition::EncodedProfile result{};
  TEST_ASSERT_TRUE(f.profile_codec.encode(value, result));
  return result;
}
inline models::EncodedModelProfile model(Fixture& f, uint32_t version) {
  models::SafetyModelProfile value{};
  value.manifest.kind = acquisition::ProfileKind::Safety;
  value.manifest.schema_version = models::kSafetyProfileSchemaVersion;
  value.manifest.profile_version = version;
  value.manifest.target_id = models::kSafetyB3TargetId;
  value.manifest.validity = acquisition::ProfileValidity::TestOnly;
  value.deployment = models::ModelDeployment::TestOnly;
  value.channel_count = 1U;
  value.channels[0] = {models::HazardSignal::TemperatureC, true, 80.0F, 70.0F};
  value.clear_consecutive_cycles = 2U;
  models::EncodedModelProfile result{};
  TEST_ASSERT_TRUE(f.model_codec.encode(value, result));
  return result;
}
inline constexpr models::ModelProfileKey model_key{acquisition::ProfileKind::Safety, models::ModelTarget::SafetyB3};
inline std::string hex(const uint8_t* bytes, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (std::size_t i = 0U; i < size; ++i) {
    result.push_back(digits[bytes[i] >> 4U]); result.push_back(digits[bytes[i] & 15U]);
  }
  return result;
}
}  // namespace admission_test
