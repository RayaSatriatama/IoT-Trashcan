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

struct ProfileManifest;
inline ProfileRef to_ref(const ProfileManifest& manifest) noexcept;

struct ProfileManifest {
  uint32_t schema_version{1};
  uint32_t profile_version{0};
  uint32_t target_id{0};
  config::ConfigHash hash{};
  ProfileValidity validity{ProfileValidity::Missing};
};

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

inline bool profile_matches(const ProfileManifest& manifest, SensorId sensor) noexcept {
  const auto target = static_cast<ProfileTarget>(manifest.target_id);
  return target == expected_profile_target(sensor);
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

struct UltrasonicProfile {
  ProfileManifest manifest{};
  float d_empty_mm{0.0F};
  float d_full_mm{0.0F};
  float echo_mm_per_us{0.0F};
  float overshoot_tolerance_mm{0.0F};
  bool valid_for_conversion() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) &&
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
  int32_t offset_centi_c{0};
  bool valid_for_conversion() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) && has_rom && rom[0] == 0x28U;
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

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
  return ProfileRef{manifest.schema_version, manifest.profile_version,
                    manifest.target_id, manifest.hash, true};
}

}  // namespace sigap::acquisition
