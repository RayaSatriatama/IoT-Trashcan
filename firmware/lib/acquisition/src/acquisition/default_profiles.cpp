#include "acquisition/default_profiles.h"

#include <cstddef>

#include "acquisition/profiles.h"

namespace sigap::acquisition {
namespace {

void initialize_manifest(ProfileManifest& manifest, ProfileKey key,
                         ProfileValidity validity) noexcept {
  manifest.kind = key.kind;
  manifest.schema_version = kProfileSchemaVersion;
  manifest.profile_version = 1U;
  manifest.target_id = static_cast<uint32_t>(key.target);
  manifest.validity = validity;
  manifest.provenance.present = true;
  constexpr char label[] = "FACTORY_DEFAULT";
  for (std::size_t i = 0U;
       i < sizeof(label) - 1U &&
       i < manifest.provenance.reference_note.size();
       ++i) {
    manifest.provenance.reference_note[i] = label[i];
  }
}

bool is_hx_target(ProfileTarget target) noexcept {
  return target == ProfileTarget::Hx711Organic ||
         target == ProfileTarget::Hx711Inorganic ||
         target == ProfileTarget::Hx711B3;
}

bool is_ultrasonic_target(ProfileTarget target) noexcept {
  return target == ProfileTarget::UltrasonicOrganic ||
         target == ProfileTarget::UltrasonicInorganic ||
         target == ProfileTarget::UltrasonicB3;
}

bool is_prototype_target(ProfileTarget target) noexcept {
  return target == ProfileTarget::Hx711Organic ||
         target == ProfileTarget::UltrasonicOrganic ||
         target == ProfileTarget::Mq135;
}

bool is_adc_target(ProfileTarget target) noexcept {
  return target == ProfileTarget::Mq4 || target == ProfileTarget::Mq135 ||
         target == ProfileTarget::Mq7 || target == ProfileTarget::Mq8;
}

}  // namespace

bool DefaultProfiles::build(ProfileTarget target,
                            EncodedProfile& output) const noexcept {
  const ProfileKey key = default_profile_key(target);
  if (key.target == ProfileTarget::Unknown) return false;

  if (target == ProfileTarget::Sht31_44 || target == ProfileTarget::Sht31_45) {
    Sht31Profile profile{};
    initialize_manifest(profile.manifest, key, ProfileValidity::Valid);
    profile.address = target == ProfileTarget::Sht31_44 ? 0x44U : 0x45U;
    return codec_.encode(profile, output);
  }

  if (is_adc_target(target)) {
    MqAdcProfile profile{};
    const bool prototype = build::kPrototypeOperation && is_prototype_target(target);
    initialize_manifest(profile.manifest, key,
                        prototype ? ProfileValidity::TestOnly : ProfileValidity::Invalid);
    if (prototype) {
      constexpr char note[] = "PROTOTYPE_DEFAULT";
      for (std::size_t i = 0U; i < sizeof(note) - 1U; ++i)
        profile.manifest.provenance.reference_note[i] = note[i];
      profile.reference_mv = prototype_defaults::kMqReferenceMv;
      profile.full_scale_counts = prototype_defaults::kMqFullScaleCounts;
      profile.divider_numerator = prototype_defaults::kMqDividerNumerator;
      profile.divider_denominator = prototype_defaults::kMqDividerDenominator;
    }
    return codec_.encode(profile, output);
  }
  if (is_hx_target(target)) {
    Hx711Profile profile{};
    const bool prototype = build::kPrototypeOperation && is_prototype_target(target);
    initialize_manifest(profile.manifest, key,
                        prototype ? ProfileValidity::TestOnly : ProfileValidity::Invalid);
    if (prototype) {
      constexpr char note[] = "PROTOTYPE_DEFAULT";
      for (std::size_t i = 0U; i < sizeof(note) - 1U; ++i)
        profile.manifest.provenance.reference_note[i] = note[i];
      profile.counts_per_kg = prototype_defaults::kHxCountsPerKg;
      profile.tare_offset_counts = prototype_defaults::kHxTareOffsetCounts;
    }
    return codec_.encode(profile, output);
  }
  if (is_ultrasonic_target(target)) {
    UltrasonicProfile profile{};
    const bool prototype = build::kPrototypeOperation && is_prototype_target(target);
    initialize_manifest(profile.manifest, key,
                        prototype ? ProfileValidity::TestOnly : ProfileValidity::Invalid);
    if (prototype) {
      constexpr char note[] = "PROTOTYPE_DEFAULT";
      for (std::size_t i = 0U; i < sizeof(note) - 1U; ++i)
        profile.manifest.provenance.reference_note[i] = note[i];
      profile.d_empty_mm = prototype_defaults::kUltrasonicEmptyMm;
      profile.d_full_mm = prototype_defaults::kUltrasonicFullMm;
      profile.echo_mm_per_us = prototype_defaults::kUltrasonicEchoMmPerUs;
      profile.overshoot_tolerance_mm = prototype_defaults::kUltrasonicOvershootToleranceMm;
    }
    profile.echo_timeout_us = UltrasonicEngineeringDefaults::kEchoTimeoutUs;
    profile.inter_sensor_gap_us = UltrasonicEngineeringDefaults::kInterSensorGapUs;
    return codec_.encode(profile, output);
  }
  if (target == ProfileTarget::Ds18b20B3) {
    Ds18b20Profile profile{};
    initialize_manifest(profile.manifest, key, ProfileValidity::Invalid);
    return codec_.encode(profile, output);
  }
  return false;
}

DefaultOutcome DefaultProfiles::bootstrap(ProfileTarget target) noexcept {
  const ProfileKey key = default_profile_key(target);
  if (key.target == ProfileTarget::Unknown) return DefaultOutcome::Invalid;

  if (target == ProfileTarget::Sht31_44 || target == ProfileTarget::Sht31_45) {
    EncodedProfile candidate{};
    if (!build(target, candidate)) return DefaultOutcome::EncodeError;
    switch (manager_.initialize_pristine(key, candidate)) {
      case FactoryProfileInitResult::Preserved: return DefaultOutcome::Preserved;
      case FactoryProfileInitResult::Initialized: return DefaultOutcome::Initialized;
      case FactoryProfileInitResult::Busy: return DefaultOutcome::Busy;
      case FactoryProfileInitResult::Invalid: return DefaultOutcome::Invalid;
      case FactoryProfileInitResult::Error: return DefaultOutcome::ReadError;
      case FactoryProfileInitResult::CommitFailed: return DefaultOutcome::CommitFailed;
    }
    return DefaultOutcome::Invalid;
  }

  if (build::kPrototypeOperation && is_prototype_target(target)) {
    EncodedProfile candidate{};
    if (!build(target, candidate)) return DefaultOutcome::EncodeError;
    if (!codec_.validate(candidate, key)) return DefaultOutcome::InvalidTemplate;
    switch (manager_.initialize_pristine(key, candidate)) {
      case FactoryProfileInitResult::Preserved: return DefaultOutcome::Preserved;
      case FactoryProfileInitResult::Initialized: return DefaultOutcome::Initialized;
      case FactoryProfileInitResult::Busy: return DefaultOutcome::Busy;
      case FactoryProfileInitResult::Invalid: return DefaultOutcome::Invalid;
      case FactoryProfileInitResult::Error: return DefaultOutcome::ReadError;
      case FactoryProfileInitResult::CommitFailed: return DefaultOutcome::CommitFailed;
    }
    return DefaultOutcome::Invalid;
  }

  EncodedProfile existing{};
  const ProfileReadStatus status = store_.read_default(key, existing);
  if (status == ProfileReadStatus::Found) {
    return DefaultOutcome::Preserved;
  }
  if (status == ProfileReadStatus::Invalid) {
    return DefaultOutcome::InvalidTemplate;
  }
  if (status != ProfileReadStatus::NotFound) return DefaultOutcome::ReadError;

  EncodedProfile candidate{};
  if (!build(target, candidate)) return DefaultOutcome::EncodeError;
  if (!codec_.validate_template(candidate, key)) return DefaultOutcome::InvalidTemplate;
  if (!store_.write_default(key, candidate)) return DefaultOutcome::WriteError;

  EncodedProfile readback{};
  if (store_.read_default(key, readback) != ProfileReadStatus::Found ||
      readback.size != candidate.size || readback.bytes != candidate.bytes ||
      readback.hash() != candidate.hash()) {
    return DefaultOutcome::ReadbackError;
  }
  return DefaultOutcome::StoredTemplate;
}

DefaultBootstrapResult DefaultProfiles::bootstrap_enabled(
    bool reference_only) noexcept {
  DefaultBootstrapResult result{};
  for (const ProfileTarget target : enabled_default_targets(reference_only)) {
    if (target == ProfileTarget::Unknown) continue;
    switch (bootstrap(target)) {
      case DefaultOutcome::Initialized: ++result.initialized; break;
      case DefaultOutcome::Preserved: ++result.preserved; break;
      case DefaultOutcome::StoredTemplate: ++result.stored; break;
      case DefaultOutcome::Busy:
      case DefaultOutcome::Invalid:
      case DefaultOutcome::InvalidTemplate: ++result.blocked; break;
      case DefaultOutcome::NotRequested: break;
      case DefaultOutcome::ReadError:
      case DefaultOutcome::WriteError:
      case DefaultOutcome::CommitFailed:
      case DefaultOutcome::ReadbackError:
      case DefaultOutcome::EncodeError: ++result.errors; break;
    }
  }
  return result;
}

}  // namespace sigap::acquisition
