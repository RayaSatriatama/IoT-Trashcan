#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "models/model_codec.h"

namespace sigap::models {

enum class ModelLoadStatus : uint8_t { Found, NotFound, Invalid, Error };
enum class ModelCommitResult : uint8_t {
  Committed,
  Invalid,
  WriteFailed,
  ReadbackFailed,
  ActivationFailed,
};

// Model activation is a separate persistence domain from acquisition
// calibration. It shares the established pointer-last A/B mechanics, while
// preserving the larger bounded model envelope and its typed key space.
enum class ModelSlot : uint8_t { A = 0U, B = 1U };

constexpr ModelSlot other_model_slot(ModelSlot slot) noexcept {
  return slot == ModelSlot::A ? ModelSlot::B : ModelSlot::A;
}

struct ModelActivationMetadata final {
  uint32_t generation{0U};
  bool has_active{false};
  ModelSlot active_slot{ModelSlot::A};
  uint32_t active_version{0U};
  config::ConfigHash active_hash{};
  bool has_previous{false};
  ModelSlot previous_slot{ModelSlot::A};
  uint32_t previous_version{0U};
  config::ConfigHash previous_hash{};
  config::ConfigHash integrity{};
};

inline constexpr uint8_t kModelActivationMetadataFormatVersion{1U};
inline constexpr std::size_t kModelActivationGenerationOffset{1U};
inline constexpr std::size_t kModelActivationHasActiveOffset{5U};
inline constexpr std::size_t kModelActivationActiveSlotOffset{6U};
inline constexpr std::size_t kModelActivationActiveVersionOffset{7U};
inline constexpr std::size_t kModelActivationActiveHashOffset{11U};
inline constexpr std::size_t kModelActivationHasPreviousOffset{43U};
inline constexpr std::size_t kModelActivationPreviousSlotOffset{44U};
inline constexpr std::size_t kModelActivationPreviousVersionOffset{45U};
inline constexpr std::size_t kModelActivationPreviousHashOffset{49U};
inline constexpr std::size_t kModelActivationCoreBytes{81U};
inline constexpr std::size_t kModelActivationIntegrityOffset{
    kModelActivationCoreBytes};
inline constexpr std::size_t kModelActivationMetadataBytes{
    kModelActivationCoreBytes + kModelHashBytes};
inline constexpr std::size_t kModelActivationCopyCount{2U};

static_assert(kModelActivationActiveHashOffset + kModelHashBytes ==
                  kModelActivationHasPreviousOffset,
              "model active metadata fields overlap");
static_assert(kModelActivationPreviousHashOffset + kModelHashBytes ==
                  kModelActivationCoreBytes,
              "model previous metadata fields overlap");
static_assert(kModelActivationIntegrityOffset + kModelHashBytes ==
                  kModelActivationMetadataBytes,
              "model metadata integrity exceeds storage blob");

class IModelSlotStore {
 public:
  virtual ~IModelSlotStore() = default;
  virtual ModelLoadStatus read_activation(
      ModelProfileKey key, ModelActivationMetadata& metadata) const noexcept = 0;
  virtual ModelLoadStatus read_slot(ModelProfileKey key, ModelSlot slot,
                                    EncodedModelProfile& encoded) const noexcept = 0;
  virtual bool write_slot(ModelProfileKey key, ModelSlot slot,
                          const EncodedModelProfile& encoded) noexcept = 0;
  virtual bool write_activation(
      ModelProfileKey key, const ModelActivationMetadata& metadata) noexcept = 0;
};

// The concrete Task 4 integration may back this interface with two model
// files plus an NVS pointer. The lifecycle depends only on pointer-last commit
// semantics and never assumes calibration ProfileCodec's 128-byte payload.
class IModelProfilePersistence {
 public:
  virtual ~IModelProfilePersistence() = default;
  virtual ModelLoadStatus load(ModelProfileKey key,
                               EncodedModelProfile& output) const noexcept = 0;
  virtual ModelCommitResult commit(
      ModelProfileKey key, const EncodedModelProfile& candidate) noexcept = 0;
  virtual ModelLoadStatus rollback(ModelProfileKey) noexcept {
    return ModelLoadStatus::Error;
  }
};

class ModelProfilePersistence final : public IModelProfilePersistence {
 public:
  explicit ModelProfilePersistence(
      IModelSlotStore& store,
      const ModelProfileCodec& codec = default_codec()) noexcept
      : store_(store), codec_(codec) {}

  ModelLoadStatus load(ModelProfileKey key,
                       EncodedModelProfile& output) const noexcept override;
  ModelCommitResult commit(
      ModelProfileKey key, const EncodedModelProfile& candidate) noexcept override;
  ModelLoadStatus rollback(ModelProfileKey key) noexcept override;

  static const ModelProfileCodec& default_codec() noexcept;

 private:
  bool load_committed(ModelProfileKey key, ModelSlot slot,
                      uint32_t version, const config::ConfigHash& hash,
                      EncodedModelProfile& output) const noexcept;

  IModelSlotStore& store_;
  const ModelProfileCodec& codec_;
};

class MemoryModelSlotStore final : public IModelSlotStore {
 public:
  enum class Fault : uint8_t {
    WriteSlot,
    CorruptAfterWrite,
    ReadSlot,
    WriteActivation,
    ReadActivation,
  };

  ModelLoadStatus read_activation(
      ModelProfileKey key, ModelActivationMetadata& metadata) const noexcept override;
  ModelLoadStatus read_slot(ModelProfileKey key, ModelSlot slot,
                            EncodedModelProfile& encoded) const noexcept override;
  bool write_slot(ModelProfileKey key, ModelSlot slot,
                  const EncodedModelProfile& encoded) noexcept override;
  bool write_activation(
      ModelProfileKey key, const ModelActivationMetadata& metadata) noexcept override;

  void fail_next(Fault fault) noexcept;
  void corrupt_slot(ModelProfileKey key, ModelSlot slot) noexcept;
  void corrupt_activation(ModelProfileKey key) noexcept;

 private:
  static constexpr std::size_t kKeyCount{4U};
  static std::size_t key_index(ModelProfileKey key) noexcept;
  static std::size_t slot_index(ModelSlot slot) noexcept {
    return slot == ModelSlot::A ? 0U : 1U;
  }

  std::array<std::array<EncodedModelProfile, 2U>, kKeyCount> slots_{};
  std::array<std::array<bool, 2U>, kKeyCount> slot_present_{};
  std::array<std::array<ModelActivationMetadata, kModelActivationCopyCount>,
             kKeyCount>
      metadata_{};
  std::array<std::array<bool, kModelActivationCopyCount>, kKeyCount>
      metadata_present_{};
  mutable Fault pending_fault_{Fault::WriteSlot};
  mutable bool fault_armed_{false};
};

// Target adapter. Native builds intentionally report storage errors rather
// than simulating NVS success; MemoryModelSlotStore covers fault injection.
class NvsModelSlotStore final : public IModelSlotStore {
 public:
  ModelLoadStatus read_activation(
      ModelProfileKey key, ModelActivationMetadata& metadata) const noexcept override;
  ModelLoadStatus read_slot(ModelProfileKey key, ModelSlot slot,
                            EncodedModelProfile& encoded) const noexcept override;
  bool write_slot(ModelProfileKey key, ModelSlot slot,
                  const EncodedModelProfile& encoded) noexcept override;
  bool write_activation(
      ModelProfileKey key, const ModelActivationMetadata& metadata) noexcept override;
};

class NvsModelProfilePersistence final : public IModelProfilePersistence {
 public:
  explicit NvsModelProfilePersistence(
      const ModelProfileCodec& codec = ModelProfilePersistence::default_codec()) noexcept
      : persistence_(store_, codec) {}

  ModelLoadStatus load(ModelProfileKey key,
                       EncodedModelProfile& output) const noexcept override {
    return persistence_.load(key, output);
  }
  ModelCommitResult commit(
      ModelProfileKey key, const EncodedModelProfile& candidate) noexcept override {
    return persistence_.commit(key, candidate);
  }
  ModelLoadStatus rollback(ModelProfileKey key) noexcept override {
    return persistence_.rollback(key);
  }

 private:
  NvsModelSlotStore store_{};
  ModelProfilePersistence persistence_;
};

enum class ModelPersistenceFault : uint8_t {
  WriteCandidate,
  CorruptAfterWrite,
  Readback,
  Activate,
  ReadActivation,
};

// Native fault-injection adapter. It models the observable A/B guarantees but
// is not a target NVS/filesystem implementation.
class FaultInjectableModelPersistence final : public IModelProfilePersistence {
 public:
  explicit FaultInjectableModelPersistence(
      const ModelProfileCodec& codec = default_codec()) noexcept
      : store_{}, persistence_(store_, codec), codec_(codec) {}

  ModelLoadStatus load(ModelProfileKey key,
                       EncodedModelProfile& output) const noexcept override;
  ModelCommitResult commit(
      ModelProfileKey key,
      const EncodedModelProfile& candidate) noexcept override;
  ModelLoadStatus rollback(ModelProfileKey key) noexcept override;
  void fail_next(ModelPersistenceFault fault) noexcept;
  void corrupt_active(ModelProfileKey key) noexcept;
  void corrupt_activation(ModelProfileKey key) noexcept;

  static const ModelProfileCodec& default_codec() noexcept;

 private:
  MemoryModelSlotStore store_{};
  ModelProfilePersistence persistence_;
  const ModelProfileCodec& codec_;
};

enum class ModelLifecycleState : uint8_t {
  Unloaded,
  Active,
  Draft,
  Validated,
  Preview,
  Error,
};

struct ModelProfileSnapshot final {
  bool present{false};
  acquisition::ProfileRef ref{};
  EncodedModelProfile encoded{};
};

class ModelProfileLifecycle final {
 public:
  ModelProfileLifecycle(IModelProfilePersistence& persistence,
                        const ModelProfileCodec& codec) noexcept
      : persistence_(persistence), codec_(codec) {}

  bool load(ModelProfileKey key) noexcept;
  bool stage(ModelProfileKey key,
             const EncodedModelProfile& encoded) noexcept;
  bool validate(ModelProfileKey key) noexcept;
  bool preview(ModelProfileKey key) noexcept;
  bool commit(ModelProfileKey key) noexcept;
  bool rollback(ModelProfileKey key) noexcept;
  bool cancel(ModelProfileKey key) noexcept;
  bool snapshot(ModelProfileKey key,
                ModelProfileSnapshot& output) const noexcept;
  ModelLifecycleState state(ModelProfileKey key) const noexcept;

 private:
  struct Entry final {
    EncodedModelProfile active{};
    EncodedModelProfile draft{};
    ModelProfileIdentity active_identity{};
    bool initialized{false};
    bool active_present{false};
    bool draft_present{false};
    ModelLifecycleState state{ModelLifecycleState::Unloaded};
  };

  static std::size_t index(ModelProfileKey key) noexcept;
  IModelProfilePersistence& persistence_;
  const ModelProfileCodec& codec_;
  std::array<Entry, 4U> entries_{};
};

}  // namespace sigap::models
