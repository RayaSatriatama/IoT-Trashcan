#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "acquisition/heater.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

// Profile persistence deliberately uses a bounded, typed envelope rather than
// persisting C++ object memory.  The hash covers the canonical header and
// payload bytes, so it is stable across ABI, padding, and reboot boundaries.
inline constexpr uint32_t kProfileMagic{0x53475031U};  // "SGP1"
inline constexpr uint16_t kProfileFormatVersion{1U};
inline constexpr uint32_t kProfileSchemaVersion{1U};
inline constexpr std::size_t kProfilePayloadBytes{128U};
inline constexpr std::size_t kProfileHeaderBytes{21U};
inline constexpr std::size_t kProfileHashBytes{32U};
inline constexpr std::size_t kEncodedProfileBytes{
    kProfileHeaderBytes + kProfilePayloadBytes + kProfileHashBytes};

enum class ProfileReadStatus : uint8_t { Found, NotFound, Invalid, Error };
enum class ProfileSlot : uint8_t { A = 0, B = 1 };

constexpr ProfileSlot other_profile_slot(ProfileSlot slot) noexcept {
  return slot == ProfileSlot::A ? ProfileSlot::B : ProfileSlot::A;
}

struct EncodedProfile final {
  std::array<uint8_t, kEncodedProfileBytes> bytes{};
  std::size_t size{0U};

  config::ConfigHash hash() const noexcept;
};

struct ProfileActivationMetadata final {
  bool has_active{false};
  ProfileSlot active_slot{ProfileSlot::A};
  uint32_t active_version{0U};
  config::ConfigHash active_hash{};
  bool has_previous{false};
  ProfileSlot previous_slot{ProfileSlot::A};
  uint32_t previous_version{0U};
  config::ConfigHash previous_hash{};
};

class IProfileSlotStore {
 public:
  virtual ~IProfileSlotStore() = default;
  virtual ProfileReadStatus read_activation(
      ProfileTarget target, ProfileActivationMetadata& metadata) const noexcept = 0;
  virtual ProfileReadStatus read_slot(ProfileTarget target, ProfileSlot slot,
                                      EncodedProfile& encoded) const noexcept = 0;
  virtual bool write_slot(ProfileTarget target, ProfileSlot slot,
                          const EncodedProfile& encoded) noexcept = 0;
  virtual bool write_activation(ProfileTarget target,
                                const ProfileActivationMetadata& metadata) noexcept = 0;
};

class NoProfileStore final : public IProfileSlotStore {
 public:
  ProfileReadStatus read_activation(ProfileTarget,
                                     ProfileActivationMetadata&) const noexcept override {
    return ProfileReadStatus::NotFound;
  }
  ProfileReadStatus read_slot(ProfileTarget, ProfileSlot,
                              EncodedProfile&) const noexcept override {
    return ProfileReadStatus::NotFound;
  }
  bool write_slot(ProfileTarget, ProfileSlot,
                  const EncodedProfile&) noexcept override { return false; }
  bool write_activation(ProfileTarget,
                        const ProfileActivationMetadata&) noexcept override {
    return false;
  }
};

class ProfileCodec final {
 public:
  bool encode(const Hx711Profile& profile, EncodedProfile& output) const noexcept;
  bool encode(const UltrasonicProfile& profile, EncodedProfile& output) const noexcept;
  bool encode(const MqAdcProfile& profile, EncodedProfile& output) const noexcept;
  bool encode(const Mq7HeaterProfile& profile, EncodedProfile& output) const noexcept;
  bool encode(const Ds18b20Profile& profile, EncodedProfile& output) const noexcept;
  bool encode(const Sht31Profile& profile, EncodedProfile& output) const noexcept;

  bool decode(const EncodedProfile& encoded, Hx711Profile& output) const noexcept;
  bool decode(const EncodedProfile& encoded, UltrasonicProfile& output) const noexcept;
  bool decode(const EncodedProfile& encoded, MqAdcProfile& output) const noexcept;
  bool decode(const EncodedProfile& encoded, Mq7HeaterProfile& output) const noexcept;
  bool decode(const EncodedProfile& encoded, Ds18b20Profile& output) const noexcept;
  bool decode(const EncodedProfile& encoded, Sht31Profile& output) const noexcept;

  bool validate(const EncodedProfile& encoded,
                ProfileTarget expected = ProfileTarget::Unknown) const noexcept;
};

enum class ProfileCommitResult : uint8_t {
  Committed,
  Invalid,
  WriteFailed,
  ReadbackFailed,
  IntegrityFailed,
  ActivationFailed,
};

class ProfilePersistence final {
 public:
  explicit ProfilePersistence(IProfileSlotStore& store,
                              const ProfileCodec& codec = default_codec()) noexcept
      : store_(store), codec_(codec) {}

  ProfileReadStatus load(ProfileTarget target,
                         EncodedProfile& encoded) const noexcept;
  ProfileCommitResult commit(ProfileTarget target,
                             const EncodedProfile& encoded) noexcept;
  ProfileReadStatus rollback(ProfileTarget target) noexcept;
  bool reset(ProfileTarget target) noexcept;

  static const ProfileCodec& default_codec() noexcept;

 private:
  bool read_identity(ProfileTarget target, ProfileSlot slot,
                     const config::ConfigHash& hash, uint32_t version,
                     EncodedProfile& encoded) const noexcept;

  IProfileSlotStore& store_;
  const ProfileCodec& codec_;
};

// Fault-injectable native store.  It models the pointer-last boundary without
// depending on Preferences/NVS, making interrupted commits testable natively.
class MemoryProfileStore final : public IProfileSlotStore {
 public:
  enum class Fault : uint8_t {
    WriteSlot,
    CorruptAfterWrite,
    ReadSlot,
    WriteActivation,
    ReadActivation,
  };

  ProfileReadStatus read_activation(ProfileTarget target,
                                     ProfileActivationMetadata& metadata) const noexcept override;
  ProfileReadStatus read_slot(ProfileTarget target, ProfileSlot slot,
                              EncodedProfile& encoded) const noexcept override;
  bool write_slot(ProfileTarget target, ProfileSlot slot,
                  const EncodedProfile& encoded) noexcept override;
  bool write_activation(ProfileTarget target,
                        const ProfileActivationMetadata& metadata) noexcept override;

  void fail_next(Fault fault) noexcept;
  void corrupt_slot(ProfileTarget target, ProfileSlot slot) noexcept;
  bool slot_present(ProfileTarget target, ProfileSlot slot) const noexcept;

 private:
  static constexpr std::size_t kTargetCount{14U};
  static std::size_t target_index(ProfileTarget target) noexcept;
  static std::size_t slot_index(ProfileSlot slot) noexcept {
    return slot == ProfileSlot::A ? 0U : 1U;
  }

  std::array<std::array<EncodedProfile, 2>, kTargetCount> slots_{};
  std::array<std::array<bool, 2>, kTargetCount> present_{};
  std::array<ProfileActivationMetadata, kTargetCount> metadata_{};
  std::array<bool, kTargetCount> metadata_present_{};
  mutable Fault pending_fault_{Fault::WriteSlot};
  mutable bool has_pending_fault_{false};
};

// Target implementation uses a separate NVS namespace and deterministic
// blobs.  Native builds intentionally expose an unavailable backend so tests
// cannot accidentally claim hardware persistence.
class NvsProfileStore final : public IProfileSlotStore {
 public:
  ProfileReadStatus read_activation(ProfileTarget target,
                                     ProfileActivationMetadata& metadata) const noexcept override;
  ProfileReadStatus read_slot(ProfileTarget target, ProfileSlot slot,
                              EncodedProfile& encoded) const noexcept override;
  bool write_slot(ProfileTarget target, ProfileSlot slot,
                  const EncodedProfile& encoded) noexcept override;
  bool write_activation(ProfileTarget target,
                        const ProfileActivationMetadata& metadata) noexcept override;
};

// Compatibility seam retained for Task 2 consumers.  It now exposes only the
// active manifest and does not imply profile persistence or a factory profile.
class IActiveProfileStore {
 public:
  virtual ~IActiveProfileStore() = default;
  virtual ProfileReadStatus read(SensorId sensor, ProfileManifest& manifest) noexcept = 0;
};

}  // namespace sigap::acquisition
