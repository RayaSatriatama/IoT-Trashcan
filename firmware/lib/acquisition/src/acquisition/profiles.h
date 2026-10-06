#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "acquisition/acquisition_types.h"

namespace sigap::acquisition {

enum class ProfileValidity : uint8_t { Missing, Invalid, Calibrating, Valid, TestOnly };

// Zero is deliberately reserved for an unbound fixture/profile. Production
// profiles and test conversion fixtures carry one sensor-specific target.
enum class ProfileTarget : uint32_t {
  Unknown = 0,
  Hx711Organic = 1,
  Hx711Inorganic = 2,
  Hx711B3 = 3,
  UltrasonicOrganic = 4,
  UltrasonicInorganic = 5,
  UltrasonicB3 = 6,
  Mq4 = 7,
  Mq135 = 8,
  Mq7 = 9,
  Mq8 = 10,
  Ds18b20B3 = 11,
  Sht31_44 = 12,
  Sht31_45 = 13,
};

struct ProfileKey final {
  ProfileKind kind{ProfileKind::Calibration};
  ProfileTarget target{ProfileTarget::Unknown};

  friend constexpr bool operator==(const ProfileKey& left,
                                   const ProfileKey& right) noexcept {
    return left.kind == right.kind && left.target == right.target;
  }
};

struct ProfileManifest;
inline ProfileRef to_ref(const ProfileManifest& manifest) noexcept;

struct ProfileManifest {
  ProfileKind kind{ProfileKind::Calibration};
  uint32_t schema_version{1};
  uint32_t profile_version{0};
  uint32_t target_id{0};
  config::ConfigHash hash{};
  ProfileValidity validity{ProfileValidity::Missing};
};

inline constexpr ProfileKind default_profile_kind(ProfileTarget target) noexcept {
  switch (target) {
    case ProfileTarget::Mq4:
    case ProfileTarget::Mq135:
    case ProfileTarget::Mq7:
    case ProfileTarget::Mq8:
      return ProfileKind::Adc;
    default:
      return ProfileKind::Calibration;
  }
}

inline constexpr ProfileKey default_profile_key(ProfileTarget target) noexcept {
  return ProfileKey{default_profile_kind(target), target};
}

inline ProfileTarget expected_profile_target(SensorId sensor) noexcept {
  switch (sensor) {
    case SensorId::Hx711Organic: return ProfileTarget::Hx711Organic;
    case SensorId::Hx711Inorganic: return ProfileTarget::Hx711Inorganic;
    case SensorId::Hx711B3: return ProfileTarget::Hx711B3;
    case SensorId::UltrasonicOrganic: return ProfileTarget::UltrasonicOrganic;
    case SensorId::UltrasonicInorganic: return ProfileTarget::UltrasonicInorganic;
    case SensorId::UltrasonicB3: return ProfileTarget::UltrasonicB3;
    case SensorId::Mq4: return ProfileTarget::Mq4;
    case SensorId::Mq135: return ProfileTarget::Mq135;
    case SensorId::Mq7: return ProfileTarget::Mq7;
    case SensorId::Mq8: return ProfileTarget::Mq8;
    case SensorId::Ds18b20B3: return ProfileTarget::Ds18b20B3;
    case SensorId::Sht31_44: return ProfileTarget::Sht31_44;
    case SensorId::Sht31_45: return ProfileTarget::Sht31_45;
  }
  return ProfileTarget::Unknown;
}

inline constexpr ProfileKind expected_profile_kind(SensorId sensor) noexcept {
  switch (sensor) {
    case SensorId::Mq4:
    case SensorId::Mq135:
    case SensorId::Mq7:
    case SensorId::Mq8:
      return ProfileKind::Adc;
    default:
      return ProfileKind::Calibration;
  }
}

inline bool profile_matches(const ProfileManifest& manifest, SensorId sensor) noexcept {
  const auto target = static_cast<ProfileTarget>(manifest.target_id);
  const bool legacy_native_fixture =
      manifest.profile_version == 0U &&
      manifest.kind == ProfileKind::Calibration;
  return target == expected_profile_target(sensor) &&
         (manifest.kind == expected_profile_kind(sensor) ||
          (legacy_native_fixture && expected_profile_kind(sensor) == ProfileKind::Adc));
}

inline bool profile_validity_allows_conversion(ProfileValidity validity) noexcept {
  return validity == ProfileValidity::Valid || validity == ProfileValidity::TestOnly;
}

struct Hx711Profile {
  ProfileManifest manifest{};
  float counts_per_kg{0.0F};
  int64_t tare_offset_counts{0};
  bool valid_for_conversion() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) &&
           std::isfinite(counts_per_kg) && counts_per_kg != 0.0F;
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

struct UltrasonicEngineeringDefaults final {
  static constexpr uint32_t kEchoTimeoutUs{30000U};
  static constexpr uint32_t kInterSensorGapUs{60000U};
};

struct UltrasonicAcquisitionParams final {
  uint32_t echo_timeout_us{UltrasonicEngineeringDefaults::kEchoTimeoutUs};
  uint32_t inter_sensor_gap_us{
      UltrasonicEngineeringDefaults::kInterSensorGapUs};
  bool profile_validated{false};
};

struct UltrasonicProfile {
  ProfileManifest manifest{};
  float d_empty_mm{0.0F};
  float d_full_mm{0.0F};
  float echo_mm_per_us{0.0F};
  float overshoot_tolerance_mm{0.0F};
  uint32_t echo_timeout_us{0U};
  uint32_t inter_sensor_gap_us{0U};
  bool valid_for_acquisition() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) &&
           echo_timeout_us > 0U && inter_sensor_gap_us > 0U;
  }
  bool valid_for_conversion() const noexcept {
    return valid_for_acquisition() &&
           std::isfinite(d_empty_mm) && std::isfinite(d_full_mm) &&
           std::isfinite(echo_mm_per_us) && std::isfinite(overshoot_tolerance_mm) &&
           d_empty_mm > 0.0F && d_full_mm > 0.0F && d_empty_mm > d_full_mm &&
           echo_mm_per_us > 0.0F && overshoot_tolerance_mm >= 0.0F;
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

struct MqAdcProfile {
  ProfileManifest manifest{};
  uint32_t reference_mv{0};
  uint32_t full_scale_counts{0};
  uint32_t divider_numerator{0};
  uint32_t divider_denominator{0};
  bool valid_for_conversion() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) &&
           reference_mv > 0U && full_scale_counts > 0U &&
           divider_numerator > 0U && divider_denominator > 0U;
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

struct Ds18b20Profile {
  ProfileManifest manifest{};
  bool has_rom{false};
  std::array<uint8_t, 8> rom{};
  uint8_t resolution_bits{12U};
  int32_t offset_centi_c{0};
  bool valid_for_acquisition() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) &&
           has_rom && rom[0] == 0x28U &&
           resolution_bits >= 9U && resolution_bits <= 12U;
  }
  bool valid_for_conversion() const noexcept {
    return valid_for_acquisition();
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

constexpr uint32_t ds18b20_conversion_time_us(
    uint8_t resolution_bits) noexcept {
  switch (resolution_bits) {
    case 9U: return 94000U;
    case 10U: return 188000U;
    case 11U: return 375000U;
    case 12U: return 750000U;
    default: return 0U;
  }
}

inline uint32_t resolve_ds18b20_conversion_time(
    const Ds18b20Profile* profile) noexcept {
  if (profile != nullptr && profile->valid_for_acquisition() &&
      profile_matches(profile->manifest, SensorId::Ds18b20B3)) {
    return ds18b20_conversion_time_us(profile->resolution_bits);
  }
  return ds18b20_conversion_time_us(12U);
}

inline UltrasonicAcquisitionParams resolve_ultrasonic_params(
    const UltrasonicProfile* profile,
    SensorId sensor = SensorId::UltrasonicOrganic) noexcept {
  if (profile != nullptr && profile->valid_for_acquisition() &&
      profile_matches(profile->manifest, sensor)) {
    return UltrasonicAcquisitionParams{
        profile->echo_timeout_us,
        profile->inter_sensor_gap_us,
        true,
    };
  }
  return UltrasonicAcquisitionParams{};
}

struct Sht31Profile {
  ProfileManifest manifest{};
  uint8_t address{0};
  int32_t temperature_offset_centi_c{0};
  int32_t humidity_offset_centi_pct{0};
  bool valid_for_conversion() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) &&
           (address == 0x44U || address == 0x45U);
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

inline ProfileRef to_ref(const ProfileManifest& manifest) noexcept {
  return ProfileRef{manifest.kind, manifest.schema_version, manifest.profile_version,
                    manifest.target_id, manifest.hash, true};
}

}  // namespace sigap::acquisition
