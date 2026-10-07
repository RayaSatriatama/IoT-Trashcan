#include "models/model_lifecycle.h"

namespace sigap::models {
namespace {

acquisition::ProfileRef to_ref(const ModelProfileIdentity& identity) noexcept {
  acquisition::ProfileRef ref{};
  ref.kind = identity.key.kind;
  ref.schema_version = identity.schema_version;
  ref.profile_version = identity.profile_version;
  ref.target_id = static_cast<uint32_t>(identity.key.target);
  ref.hash = identity.hash;
  ref.present = true;
  return ref;
}

}  // namespace

std::size_t FaultInjectableModelPersistence::index(
    ModelProfileKey key) noexcept {
  if (key.kind == acquisition::ProfileKind::Fis) {
    if (key.target == ModelTarget::FisOrganic) return 0U;
    if (key.target == ModelTarget::FisInorganic) return 1U;
    if (key.target == ModelTarget::FisB3) return 2U;
  }
  if (key.kind == acquisition::ProfileKind::Safety &&
      key.target == ModelTarget::SafetyB3) return 3U;
  return 4U;
}

const ModelProfileCodec& FaultInjectableModelPersistence::default_codec() noexcept {
  static const ModelProfileCodec codec{};
  return codec;
}

ModelLoadStatus FaultInjectableModelPersistence::load(
    ModelProfileKey key, EncodedModelProfile& output) const noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return ModelLoadStatus::Error;
  const auto& entry = entries_[entry_index];
  if (entry.active_present && codec_.validate(entry.active, key)) {
    output = entry.active;
    return ModelLoadStatus::Found;
  }
  if (entry.previous_present && codec_.validate(entry.previous, key)) {
    output = entry.previous;
    return ModelLoadStatus::Found;
  }
  output = EncodedModelProfile{};
  return entry.active_present || entry.previous_present
             ? ModelLoadStatus::Invalid
             : ModelLoadStatus::NotFound;
}

ModelCommitResult FaultInjectableModelPersistence::commit(
    ModelProfileKey key, const EncodedModelProfile& candidate) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size() || !codec_.validate(candidate, key)) {
    return ModelCommitResult::Invalid;
  }
  auto& entry = entries_[entry_index];
  if (fault_armed_ && next_fault_ == ModelPersistenceFault::WriteCandidate) {
    fault_armed_ = false;
    return ModelCommitResult::WriteFailed;
  }
  entry.staged = candidate;
  entry.staged_present = true;
  if (fault_armed_ && next_fault_ == ModelPersistenceFault::Readback) {
    fault_armed_ = false;
    return ModelCommitResult::ReadbackFailed;
  }
  if (!codec_.validate(entry.staged, key)) {
    return ModelCommitResult::ReadbackFailed;
  }
  if (fault_armed_ && next_fault_ == ModelPersistenceFault::Activate) {
    fault_armed_ = false;
    return ModelCommitResult::ActivationFailed;
  }
  if (entry.active_present) {
    entry.previous = entry.active;
    entry.previous_present = true;
  }
  entry.active = entry.staged;
  entry.active_present = true;
  entry.staged = EncodedModelProfile{};
  entry.staged_present = false;
  return ModelCommitResult::Committed;
}

void FaultInjectableModelPersistence::fail_next(
    ModelPersistenceFault fault) noexcept {
  next_fault_ = fault;
  fault_armed_ = true;
}

void FaultInjectableModelPersistence::corrupt_active(
    ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size() || !entries_[entry_index].active_present ||
      entries_[entry_index].active.size == 0U) return;
  entries_[entry_index].active.bytes[0] ^= 0x01U;
}

std::size_t ModelProfileLifecycle::index(ModelProfileKey key) noexcept {
  if (key.kind == acquisition::ProfileKind::Fis) {
    if (key.target == ModelTarget::FisOrganic) return 0U;
    if (key.target == ModelTarget::FisInorganic) return 1U;
    if (key.target == ModelTarget::FisB3) return 2U;
  }
  if (key.kind == acquisition::ProfileKind::Safety &&
      key.target == ModelTarget::SafetyB3) return 3U;
  return 4U;
}

bool ModelProfileLifecycle::load(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  EncodedModelProfile encoded{};
  const auto status = persistence_.load(key, encoded);
  entry = Entry{};
  entry.initialized = true;
  if (status == ModelLoadStatus::NotFound) return true;
  if (status != ModelLoadStatus::Found ||
      !codec_.identity(encoded, entry.active_identity) ||
      !(entry.active_identity.key == key) || !codec_.validate(encoded, key)) {
    entry.state = ModelLifecycleState::Error;
    return false;
  }
  entry.active = encoded;
  entry.active_present = true;
  entry.state = ModelLifecycleState::Active;
  return true;
}

bool ModelProfileLifecycle::stage(
    ModelProfileKey key, const EncodedModelProfile& encoded) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size() || !codec_.validate(encoded, key)) {
    return false;
  }
  auto& entry = entries_[entry_index];
  if (!entry.initialized) return false;
  ModelProfileIdentity candidate{};
  if (!codec_.identity(encoded, candidate) || !(candidate.key == key) ||
      (entry.active_present &&
       candidate.profile_version <= entry.active_identity.profile_version)) {
    return false;
  }
  entry.draft = encoded;
  entry.draft_present = true;
  entry.state = ModelLifecycleState::Draft;
  return true;
}

bool ModelProfileLifecycle::validate(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  if (!entry.draft_present || !codec_.validate(entry.draft, key)) return false;
  entry.state = ModelLifecycleState::Validated;
  return true;
}

bool ModelProfileLifecycle::preview(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  if (entry.state != ModelLifecycleState::Validated ||
      !entry.draft_present) return false;
  entry.state = ModelLifecycleState::Preview;
  return true;
}

bool ModelProfileLifecycle::commit(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  if (entry.state != ModelLifecycleState::Preview ||
      !entry.draft_present) return false;
  if (persistence_.commit(key, entry.draft) != ModelCommitResult::Committed) {
    return false;
  }
  ModelProfileIdentity identity{};
  if (!codec_.identity(entry.draft, identity)) {
    entry.state = ModelLifecycleState::Error;
    return false;
  }
  entry.active = entry.draft;
  entry.active_identity = identity;
  entry.active_present = true;
  entry.draft = EncodedModelProfile{};
  entry.draft_present = false;
  entry.state = ModelLifecycleState::Active;
  return true;
}

bool ModelProfileLifecycle::cancel(ModelProfileKey key) noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  auto& entry = entries_[entry_index];
  entry.draft = EncodedModelProfile{};
  entry.draft_present = false;
  entry.state = entry.active_present ? ModelLifecycleState::Active
                                     : ModelLifecycleState::Unloaded;
  return true;
}

bool ModelProfileLifecycle::snapshot(
    ModelProfileKey key, ModelProfileSnapshot& output) const noexcept {
  const auto entry_index = index(key);
  if (entry_index >= entries_.size()) return false;
  const auto& entry = entries_[entry_index];
  output = ModelProfileSnapshot{};
  if (!entry.active_present) return true;
  output.present = true;
  output.ref = to_ref(entry.active_identity);
  output.encoded = entry.active;
  return true;
}

ModelLifecycleState ModelProfileLifecycle::state(
    ModelProfileKey key) const noexcept {
  const auto entry_index = index(key);
  return entry_index < entries_.size() ? entries_[entry_index].state
                                       : ModelLifecycleState::Error;
}

}  // namespace sigap::models
