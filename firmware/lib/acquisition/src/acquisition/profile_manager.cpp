#include "acquisition/profile_manager.h"

namespace sigap::acquisition {

std::size_t ProfileManager::index(ProfileTarget target) noexcept {
  return key_index(default_profile_key(target));
}

bool ProfileManager::valid_key(ProfileKey key) noexcept {
  const auto target = static_cast<uint32_t>(key.target);
  if (key.kind == ProfileKind::Calibration) {
    return (target >= 1U && target <= 6U) || (target >= 11U && target <= 13U);
  }
  if (key.kind == ProfileKind::Adc) return target >= 7U && target <= 10U;
  return key.kind == ProfileKind::Heater && key.target == ProfileTarget::Mq7;
}

std::size_t ProfileManager::key_index(ProfileKey key) noexcept {
  const auto target = static_cast<uint32_t>(key.target);
  if (key.kind == ProfileKind::Calibration) {
    if (target >= 1U && target <= 6U) return target - 1U;
    if (target == 11U) return 6U;
    if (target == 12U) return 7U;
    if (target == 13U) return 8U;
  } else if (key.kind == ProfileKind::Adc && target >= 7U && target <= 10U) {
    return 9U + target - 7U;
  } else if (key.kind == ProfileKind::Heater && key.target == ProfileTarget::Mq7) {
    return 13U;
  }
  return kTargetCount;
}

ProfileRef ProfileManager::ref_from(const EncodedProfile& encoded) noexcept {
  ProfileRef ref{};
  if (encoded.size < kProfileHeaderBytes) return ref;
  ref.hash = encoded.hash();
  bool nonzero_hash{false};
  for (const uint8_t byte : ref.hash) nonzero_hash = nonzero_hash || byte != 0U;
  ref.present = nonzero_hash;
  if (!ref.present) return ref;
  // Envelope fields are little-endian and fixed by ProfileCodec.  Reading
  // these identity fields explicitly avoids persisting or exposing structs.
  ref.schema_version = static_cast<uint32_t>(encoded.bytes[6]) |
                       static_cast<uint32_t>(encoded.bytes[7]) << 8U |
                       static_cast<uint32_t>(encoded.bytes[8]) << 16U |
                       static_cast<uint32_t>(encoded.bytes[9]) << 24U;
  ref.target_id = static_cast<uint32_t>(encoded.bytes[10]) |
                  static_cast<uint32_t>(encoded.bytes[11]) << 8U |
                  static_cast<uint32_t>(encoded.bytes[12]) << 16U |
                  static_cast<uint32_t>(encoded.bytes[13]) << 24U;
  ref.profile_version = static_cast<uint32_t>(encoded.bytes[14]) |
                        static_cast<uint32_t>(encoded.bytes[15]) << 8U |
                        static_cast<uint32_t>(encoded.bytes[16]) << 16U |
                        static_cast<uint32_t>(encoded.bytes[17]) << 24U;
  ref.kind = static_cast<ProfileKind>(encoded.bytes[18]);
  return ref;
}

bool ProfileManager::load(ProfileKey key) noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  EncodedProfile loaded{};
  const auto result = persistence_.load(key, loaded);
  active_present_[target_index] = result == ProfileReadStatus::Found;
  draft_present_[target_index] = false;
  if (active_present_[target_index]) {
    active_[target_index] = loaded;
    states_[target_index] = ProfileManagerState::Active;
    sync_runtime_key(key);
    ++generation_;
    return true;
  }
  states_[target_index] = result == ProfileReadStatus::Error
                              ? ProfileManagerState::Error
                              : ProfileManagerState::Unloaded;
  sync_runtime_key(key);
  return result == ProfileReadStatus::NotFound;
}

bool ProfileManager::ensure_loaded(ProfileTarget target) noexcept {
  return ensure_loaded(default_profile_key(target));
}

bool ProfileManager::ensure_loaded(ProfileKey key) noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  if (states_[target_index] != ProfileManagerState::Unloaded) return true;
  return load(key);
}

bool ProfileManager::stage_import(ProfileKey key,
                                  const EncodedProfile& encoded) noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  if (!codec_.validate(encoded, key)) return false;
  if (!ensure_loaded(key)) return false;
  draft_[target_index] = encoded;
  draft_present_[target_index] = true;
  states_[target_index] = ProfileManagerState::Draft;
  return true;
}

bool ProfileManager::validate(ProfileKey key) const noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  return draft_present_[target_index] && codec_.validate(draft_[target_index], key);
}

bool ProfileManager::preview(ProfileKey key) noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  if (!validate(key)) return false;
  states_[target_index] = ProfileManagerState::Preview;
  return true;
}

bool ProfileManager::commit(ProfileKey key) noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  if (states_[target_index] != ProfileManagerState::Preview ||
      !validate(key)) return false;
  const ProfileRef previous = active_present_[target_index]
                                  ? ref_from(active_[target_index])
                                  : ProfileRef{};
  if (persistence_.commit(key, draft_[target_index]) != ProfileCommitResult::Committed) {
    states_[target_index] = ProfileManagerState::Draft;
    return false;
  }
  active_[target_index] = draft_[target_index];
  active_present_[target_index] = true;
  draft_present_[target_index] = false;
  states_[target_index] = ProfileManagerState::Active;
  sync_runtime_key(key);
  ++generation_;
  if (audit_ != nullptr) {
    audit_->set_context(audit_monotonic_us_, audit_config_ref_);
    audit_->profile_activated(previous, ref_from(active_[target_index]));
  }
  return true;
}

bool ProfileManager::rollback(ProfileKey key) noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  const ProfileRef previous = active_present_[target_index]
                                  ? ref_from(active_[target_index])
                                  : ProfileRef{};
  if (persistence_.rollback(key) != ProfileReadStatus::Found || !load(key)) return false;
  if (audit_ != nullptr) {
    audit_->set_context(audit_monotonic_us_, audit_config_ref_);
    audit_->profile_rolled_back(previous, ref_from(active_[target_index]));
  }
  return true;
}

bool ProfileManager::reset(ProfileKey key) noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  const ProfileRef previous = active_present_[target_index]
                                  ? ref_from(active_[target_index])
                                  : ProfileRef{};
  if (!persistence_.reset(key)) return false;
  active_present_[target_index] = false;
  draft_present_[target_index] = false;
  active_[target_index] = EncodedProfile{};
  draft_[target_index] = EncodedProfile{};
  states_[target_index] = ProfileManagerState::Unloaded;
  sync_runtime_key(key);
  ++generation_;
  if (audit_ != nullptr) {
    audit_->set_context(audit_monotonic_us_, audit_config_ref_);
    audit_->profile_reset(previous);
  }
  return true;
}

bool ProfileManager::export_active(ProfileKey key,
                                   EncodedProfile& encoded) const noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  if (!active_present_[target_index]) return false;
  encoded = active_[target_index];
  return true;
}

bool ProfileManager::snapshot(ProfileKey key,
                              ActiveProfileSnapshot& snapshot) const noexcept {
  const auto target_index = key_index(key);
  if (!valid_key(key) || target_index >= kTargetCount) return false;
  snapshot = ActiveProfileSnapshot{};
  if (!active_present_[target_index]) return true;
  snapshot.present = true;
  snapshot.encoded = active_[target_index];
  snapshot.ref = ref_from(active_[target_index]);
  return true;
}

ProfileManagerState ProfileManager::state(ProfileKey key) const noexcept {
  if (!valid_key(key)) return ProfileManagerState::Error;
  return states_[key_index(key)];
}

void ProfileManager::bind_runtime_profiles(TargetProfileSet& profiles) noexcept {
  runtime_profiles_ = &profiles;
  profiles = TargetProfileSet{};
  for (uint32_t target = 1U; target < kTargetCount; ++target) {
    sync_runtime_target(static_cast<ProfileTarget>(target));
  }
  sync_runtime_key(ProfileKey{ProfileKind::Heater, ProfileTarget::Mq7});
}

void ProfileManager::sync_runtime_target(ProfileTarget target) noexcept {
  sync_runtime_key(default_profile_key(target));
}

void ProfileManager::sync_runtime_key(ProfileKey key) noexcept {
  if (runtime_profiles_ == nullptr || !valid_key(key)) return;
  const auto target = key.target;
  const auto target_index = key_index(key);
  const bool present = active_present_[target_index];
  if (key.kind == ProfileKind::Heater && target == ProfileTarget::Mq7) {
    heater_runtime_ = Mq7HeaterProfile{};
    if (present) (void)codec_.decode(active_[target_index], heater_runtime_);
    runtime_profiles_->heater = &heater_runtime_;
    return;
  }
  if (key.kind == ProfileKind::Adc &&
      target >= ProfileTarget::Mq4 && target <= ProfileTarget::Mq8) {
    const std::size_t slot = static_cast<std::size_t>(target) - 7U;
    mq_runtime_[slot] = MqAdcProfile{};
    if (present) (void)codec_.decode(active_[target_index], mq_runtime_[slot]);
    runtime_profiles_->mq_adc[slot] = &mq_runtime_[slot];
    return;
  }
  switch (target) {
    case ProfileTarget::Hx711Organic:
    case ProfileTarget::Hx711Inorganic:
    case ProfileTarget::Hx711B3: {
      const std::size_t slot = static_cast<std::size_t>(target) - 1U;
      hx711_runtime_[slot] = Hx711Profile{};
      if (present && codec_.decode(active_[target_index], hx711_runtime_[slot]))
        runtime_profiles_->hx711[slot] = &hx711_runtime_[slot];
      else runtime_profiles_->hx711[slot] = &hx711_runtime_[slot];
      break;
    }
    case ProfileTarget::UltrasonicOrganic:
    case ProfileTarget::UltrasonicInorganic:
    case ProfileTarget::UltrasonicB3: {
      const std::size_t slot = static_cast<std::size_t>(target) - 4U;
      ultrasonic_runtime_[slot] = UltrasonicProfile{};
      if (present && codec_.decode(active_[target_index], ultrasonic_runtime_[slot]))
        runtime_profiles_->ultrasonic[slot] = &ultrasonic_runtime_[slot];
      else runtime_profiles_->ultrasonic[slot] = &ultrasonic_runtime_[slot];
      break;
    }
    case ProfileTarget::Mq4:
    case ProfileTarget::Mq135:
    case ProfileTarget::Mq7:
    case ProfileTarget::Mq8: {
      const std::size_t slot = static_cast<std::size_t>(target) - 7U;
      mq_runtime_[slot] = MqAdcProfile{};
      if (present && codec_.decode(active_[target_index], mq_runtime_[slot]))
        runtime_profiles_->mq_adc[slot] = &mq_runtime_[slot];
      else runtime_profiles_->mq_adc[slot] = &mq_runtime_[slot];
      break;
    }
    case ProfileTarget::Ds18b20B3:
      ds_runtime_ = Ds18b20Profile{};
      if (present && codec_.decode(active_[target_index], ds_runtime_))
        runtime_profiles_->ds18b20 = &ds_runtime_;
      else runtime_profiles_->ds18b20 = &ds_runtime_;
      break;
    case ProfileTarget::Sht31_44:
    case ProfileTarget::Sht31_45: {
      const std::size_t slot = target == ProfileTarget::Sht31_44 ? 0U : 1U;
      sht_runtime_[slot] = Sht31Profile{};
      if (present && codec_.decode(active_[target_index], sht_runtime_[slot]))
        runtime_profiles_->sht31[slot] = &sht_runtime_[slot];
      else runtime_profiles_->sht31[slot] = &sht_runtime_[slot];
      break;
    }
    case ProfileTarget::Unknown: break;
  }
}

}  // namespace sigap::acquisition
