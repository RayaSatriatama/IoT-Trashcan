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
};

enum class ModelPersistenceFault : uint8_t {
  WriteCandidate,
  Readback,
  Activate,
};

// Native fault-injection adapter. It models the observable A/B guarantees but
// is not a target NVS/filesystem implementation.
class FaultInjectableModelPersistence final : public IModelProfilePersistence {
 public:
  explicit FaultInjectableModelPersistence(
      const ModelProfileCodec& codec = default_codec()) noexcept
      : codec_(codec) {}

  ModelLoadStatus load(ModelProfileKey key,
                       EncodedModelProfile& output) const noexcept override;
  ModelCommitResult commit(
      ModelProfileKey key,
      const EncodedModelProfile& candidate) noexcept override;
  void fail_next(ModelPersistenceFault fault) noexcept;
  void corrupt_active(ModelProfileKey key) noexcept;

  static const ModelProfileCodec& default_codec() noexcept;

 private:
  struct Entry final {
    EncodedModelProfile active{};
    EncodedModelProfile previous{};
    EncodedModelProfile staged{};
    bool active_present{false};
    bool previous_present{false};
    bool staged_present{false};
  };

  static std::size_t index(ModelProfileKey key) noexcept;
  std::array<Entry, 4U> entries_{};
  const ModelProfileCodec& codec_;
  ModelPersistenceFault next_fault_{ModelPersistenceFault::WriteCandidate};
  bool fault_armed_{false};
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
