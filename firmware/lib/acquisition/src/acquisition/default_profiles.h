#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "acquisition/profile_manager.h"
#include "sigap/build/build_config.h"

namespace sigap::acquisition {

namespace prototype_defaults {
inline constexpr float kHxCountsPerKg{100000.0F};
inline constexpr int64_t kHxTareOffsetCounts{-625067LL};
inline constexpr float kUltrasonicEchoMmPerUs{0.1715F};
inline constexpr float kUltrasonicEmptyMm{230.153F};
inline constexpr float kUltrasonicFullMm{20.0F};
inline constexpr float kUltrasonicOvershootToleranceMm{50.0F};
inline constexpr uint32_t kMqReferenceMv{3300U};
inline constexpr uint32_t kMqFullScaleCounts{4095U};
inline constexpr uint32_t kMqDividerNumerator{1U};
inline constexpr uint32_t kMqDividerDenominator{1U};
}  // namespace prototype_defaults


enum class DefaultOutcome : uint8_t {
  NotRequested,
  Preserved,
  Initialized,
  StoredTemplate,
  InvalidTemplate,
  Busy,
  Invalid,
  ReadError,
  WriteError,
  CommitFailed,
  ReadbackError,
  EncodeError,
};

struct DefaultBootstrapResult final {
  std::size_t initialized{0U};
  std::size_t preserved{0U};
  std::size_t stored{0U};
  std::size_t blocked{0U};
  std::size_t errors{0U};
};

constexpr std::array<ProfileTarget, 14U> enabled_default_targets(
    bool reference_only) noexcept {
  return {ProfileTarget::Hx711Organic,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::Hx711Inorganic,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::Hx711B3,
          ProfileTarget::UltrasonicOrganic,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::UltrasonicInorganic,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::UltrasonicB3,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::Mq4,
          ProfileTarget::Mq135,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::Mq7,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::Mq8,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::Ds18b20B3,
          ProfileTarget::Sht31_44,
          reference_only ? ProfileTarget::Unknown : ProfileTarget::Sht31_45,
          ProfileTarget::Unknown};
}

class DefaultProfiles final {
 public:
  explicit DefaultProfiles(
      ProfileManager& manager, IProfileSlotStore& store,
      const ProfileCodec& codec = ProfilePersistence::default_codec()) noexcept
      : manager_(manager), store_(store), codec_(codec) {}

  bool build(ProfileTarget target, EncodedProfile& output) const noexcept;
  DefaultOutcome bootstrap(ProfileTarget target) noexcept;
  DefaultBootstrapResult bootstrap_enabled(bool reference_only) noexcept;

 private:
  ProfileManager& manager_;
  IProfileSlotStore& store_;
  const ProfileCodec& codec_;
};

}  // namespace sigap::acquisition
