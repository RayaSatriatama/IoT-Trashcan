#pragma once

#include <array>
#include <cstdint>

#include "acquisition/acquisition_types.h"

namespace sigap::acquisition {

enum class ProfileValidity : uint8_t { Missing, Invalid, Calibrating, Valid, TestOnly };

struct ProfileManifest;
inline ProfileRef to_ref(const ProfileManifest& manifest) noexcept;

struct ProfileManifest {
  uint32_t schema_version{1};
  uint32_t profile_version{0};
  uint32_t target_id{0};
  config::ConfigHash hash{};
  ProfileValidity validity{ProfileValidity::Missing};
};

struct Hx711Profile {
  ProfileManifest manifest{};
  float counts_per_kg{0.0F};
  float offset_counts{0.0F};
  bool valid_for_conversion() const noexcept {
    return (manifest.validity == ProfileValidity::Valid ||
            manifest.validity == ProfileValidity::TestOnly) && counts_per_kg > 0.0F;
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

struct UltrasonicProfile {
  ProfileManifest manifest{};
  float d_empty_mm{0.0F};
  float d_full_mm{0.0F};
  float echo_mm_per_us{0.0F};
  bool valid_for_conversion() const noexcept {
    return (manifest.validity == ProfileValidity::Valid ||
            manifest.validity == ProfileValidity::TestOnly) &&
           d_empty_mm > 0.0F && d_full_mm > 0.0F && d_empty_mm > d_full_mm &&
           echo_mm_per_us > 0.0F;
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
    return (manifest.validity == ProfileValidity::Valid ||
            manifest.validity == ProfileValidity::TestOnly) &&
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
    return (manifest.validity == ProfileValidity::Valid ||
            manifest.validity == ProfileValidity::TestOnly) && has_rom && rom[0] == 0x28U;
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

struct Sht31Profile {
  ProfileManifest manifest{};
  uint8_t address{0};
  int32_t temperature_offset_centi_c{0};
  int32_t humidity_offset_centi_pct{0};
  bool valid_for_conversion() const noexcept {
    return (manifest.validity == ProfileValidity::Valid ||
            manifest.validity == ProfileValidity::TestOnly) &&
           (address == 0x44U || address == 0x45U);
  }
  ProfileRef ref() const noexcept { return to_ref(manifest); }
};

inline ProfileRef to_ref(const ProfileManifest& manifest) noexcept {
  return ProfileRef{manifest.schema_version, manifest.profile_version,
                    manifest.target_id, manifest.hash, true};
}

}  // namespace sigap::acquisition
