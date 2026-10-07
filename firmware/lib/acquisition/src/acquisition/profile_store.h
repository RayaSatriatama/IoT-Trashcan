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
inline constexpr std::size_t kProfileHeaderBytes{22U};
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
  uint32_t generation{0U};
  bool has_active{false};
  ProfileSlot active_slot{ProfileSlot::A};
  uint32_t active_version{0U};
  config::ConfigHash active_hash{};
  bool has_previous{false};
  ProfileSlot previous_slot{ProfileSlot::A};
  uint32_t previous_version{0U};
  config::ConfigHash previous_hash{};
  config::ConfigHash integrity{};
};

// Versioned metadata is stored as a complete bounded blob. Keep all offsets
// in one definition so the native and NVS implementations cannot drift.
inline constexpr uint8_t kActivationMetadataFormatVersion{2U};
inline constexpr std::size_t kActivationMetadataGenerationOffset{1U};
inline constexpr std::size_t kActivationMetadataHasActiveOffset{5U};
inline constexpr std::size_t kActivationMetadataActiveSlotOffset{6U};
inline constexpr std::size_t kActivationMetadataActiveVersionOffset{7U};
inline constexpr std::size_t kActivationMetadataActiveHashOffset{11U};
inline constexpr std::size_t kActivationMetadataHasPreviousOffset{43U};
inline constexpr std::size_t kActivationMetadataPreviousSlotOffset{44U};
inline constexpr std::size_t kActivationMetadataPreviousVersionOffset{45U};
inline constexpr std::size_t kActivationMetadataPreviousHashOffset{49U};
inline constexpr std::size_t kActivationMetadataCoreBytes{81U};
inline constexpr std::size_t kActivationMetadataIntegrityOffset{
    kActivationMetadataCoreBytes};
inline constexpr std::size_t kActivationMetadataBytes{
    kActivationMetadataCoreBytes + kProfileHashBytes};
inline constexpr std::size_t kActivationMetadataCopyCount{2U};
static_assert(kActivationMetadataActiveHashOffset + kProfileHashBytes ==
                  kActivationMetadataHasPreviousOffset,
              "active metadata fields overlap");
static_assert(kActivationMetadataGenerationOffset + sizeof(uint32_t) <=
                  kActivationMetadataHasActiveOffset,
              "generation metadata field overlaps");
static_assert(kActivationMetadataHasActiveOffset + 1U <=
                  kActivationMetadataActiveSlotOffset,
              "active flag metadata field overlaps");
static_assert(kActivationMetadataActiveSlotOffset + 1U <=
                  kActivationMetadataActiveVersionOffset,
              "active slot metadata field overlaps");
static_assert(kActivationMetadataActiveVersionOffset + sizeof(uint32_t) <=
                  kActivationMetadataActiveHashOffset,
              "active version metadata field overlaps");
static_assert(kActivationMetadataHasPreviousOffset + 1U <=
                  kActivationMetadataPreviousSlotOffset,
              "previous flag metadata field overlaps");
static_assert(kActivationMetadataPreviousSlotOffset + 1U <=
                  kActivationMetadataPreviousVersionOffset,
              "previous slot metadata field overlaps");
static_assert(kActivationMetadataPreviousVersionOffset + sizeof(uint32_t) <=
                  kActivationMetadataPreviousHashOffset,
              "previous version metadata field overlaps");
static_assert(kActivationMetadataPreviousHashOffset + kProfileHashBytes ==
                  kActivationMetadataCoreBytes,
              "previous metadata fields overlap");
static_assert(kActivationMetadataIntegrityOffset + kProfileHashBytes ==
                  kActivationMetadataBytes,
              "metadata integrity exceeds storage blob");
static_assert(sizeof(ProfileActivationMetadata) >= kProfileHashBytes * 3U,
              "metadata object unexpectedly lost integrity fields");

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

  // Key-aware seams are additive so Task 2 target-only callers remain source
  // compatible while Task 4 keeps independent ADC and HEATER records.
  virtual ProfileReadStatus read_activation(
      ProfileKey key, ProfileActivationMetadata& metadata) const noexcept {
    return read_activation(key.target, metadata);
  }
  virtual ProfileReadStatus read_slot(ProfileKey key, ProfileSlot slot,
                                      EncodedProfile& encoded) const noexcept {
    return read_slot(key.target, slot, encoded);
  }
  virtual bool write_slot(ProfileKey key, ProfileSlot slot,
                          const EncodedProfile& encoded) noexcept {
    return write_slot(key.target, slot, encoded);
  }
  virtual bool write_activation(
      ProfileKey key, const ProfileActivationMetadata& metadata) noexcept {
    return write_activation(key.target, metadata);
  }
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
  bool validate(const EncodedProfile& encoded,
                ProfileKey expected) const noexcept;
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

  ProfileReadStatus load(ProfileKey key,
                         EncodedProfile& encoded) const noexcept;
  ProfileCommitResult commit(ProfileKey key,
                             const EncodedProfile& encoded) noexcept;
  ProfileReadStatus rollback(ProfileKey key) noexcept;
  bool reset(ProfileKey key) noexcept;

  ProfileReadStatus load(ProfileTarget target,
                         EncodedProfile& encoded) const noexcept {
    return load(default_profile_key(target), encoded);
  }
  ProfileCommitResult commit(ProfileTarget target,
                             const EncodedProfile& encoded) noexcept {
    return commit(default_profile_key(target), encoded);
  }
  ProfileReadStatus rollback(ProfileTarget target) noexcept {
    return rollback(default_profile_key(target));
  }
  bool reset(ProfileTarget target) noexcept {
    return reset(default_profile_key(target));
  }

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
  static std::size_t key_index(ProfileKey key) noexcept;
  static std::size_t slot_index(ProfileSlot slot) noexcept {
    return slot == ProfileSlot::A ? 0U : 1U;
  }

  std::array<std::array<EncodedProfile, 2>, kTargetCount> slots_{};
  std::array<std::array<bool, 2>, kTargetCount> present_{};
  std::array<std::array<ProfileActivationMetadata, kActivationMetadataCopyCount>, kTargetCount>
      metadata_{};
  std::array<std::array<bool, kActivationMetadataCopyCount>, kTargetCount> metadata_present_{};
  mutable Fault pending_fault_{Fault::WriteSlot};
  mutable bool has_pending_fault_{false};

 public:
  ProfileReadStatus read_activation(ProfileKey key,
                                     ProfileActivationMetadata& metadata) const noexcept override;
  ProfileReadStatus read_slot(ProfileKey key, ProfileSlot slot,
                              EncodedProfile& encoded) const noexcept override;
  bool write_slot(ProfileKey key, ProfileSlot slot,
                  const EncodedProfile& encoded) noexcept override;
  bool write_activation(ProfileKey key,
                        const ProfileActivationMetadata& metadata) noexcept override;
  void corrupt_activation(ProfileKey key) noexcept;
  void corrupt_activation_copy(ProfileKey key, std::size_t copy) noexcept;
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
  ProfileReadStatus read_activation(ProfileKey key,
                                    ProfileActivationMetadata& metadata) const noexcept override;
  ProfileReadStatus read_slot(ProfileKey key, ProfileSlot slot,
                              EncodedProfile& encoded) const noexcept override;
  bool write_slot(ProfileKey key, ProfileSlot slot,
                  const EncodedProfile& encoded) noexcept override;
  bool write_activation(ProfileKey key,
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
