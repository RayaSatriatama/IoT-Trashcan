#include "acquisition/profile_manager.h"

namespace sigap::acquisition {

std::size_t ProfileManager::index(ProfileTarget target) noexcept {
  const auto value = static_cast<uint32_t>(target);
  return value < kTargetCount ? static_cast<std::size_t>(value) : 0U;
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
  return ref;
}

bool ProfileManager::load(ProfileTarget target) noexcept {
  const auto target_index = index(target);
  EncodedProfile loaded{};
  const auto result = persistence_.load(target, loaded);
  active_present_[target_index] = result == ProfileReadStatus::Found;
  draft_present_[target_index] = false;
  if (active_present_[target_index]) {
    active_[target_index] = loaded;
    states_[target_index] = ProfileManagerState::Active;
    sync_runtime_target(target);
    ++generation_;
    return true;
  }
  states_[target_index] = result == ProfileReadStatus::Error
                              ? ProfileManagerState::Error
                              : ProfileManagerState::Unloaded;
  sync_runtime_target(target);
  return result == ProfileReadStatus::NotFound;
}

bool ProfileManager::ensure_loaded(ProfileTarget target) noexcept {
  const auto target_index = index(target);
  if (states_[target_index] != ProfileManagerState::Unloaded) return true;
  return load(target);
}

bool ProfileManager::stage_import(ProfileTarget target,
                                  const EncodedProfile& encoded) noexcept {
  const auto target_index = index(target);
  if (!codec_.validate(encoded, target)) return false;
  if (!ensure_loaded(target)) return false;
  draft_[target_index] = encoded;
  draft_present_[target_index] = true;
  states_[target_index] = ProfileManagerState::Draft;
  return true;
}

bool ProfileManager::validate(ProfileTarget target) const noexcept {
  const auto target_index = index(target);
  return draft_present_[target_index] && codec_.validate(draft_[target_index], target);
}

bool ProfileManager::preview(ProfileTarget target) noexcept {
  const auto target_index = index(target);
  if (!validate(target)) return false;
  states_[target_index] = ProfileManagerState::Preview;
  return true;
}

bool ProfileManager::commit(ProfileTarget target) noexcept {
  const auto target_index = index(target);
  if (states_[target_index] != ProfileManagerState::Preview ||
      !validate(target)) return false;
  const ProfileRef previous = active_present_[target_index]
                                  ? ref_from(active_[target_index])
                                  : ProfileRef{};
  if (persistence_.commit(target, draft_[target_index]) != ProfileCommitResult::Committed) {
    states_[target_index] = ProfileManagerState::Draft;
    return false;
  }
  active_[target_index] = draft_[target_index];
  active_present_[target_index] = true;
  draft_present_[target_index] = false;
  states_[target_index] = ProfileManagerState::Active;
  sync_runtime_target(target);
  ++generation_;
  if (audit_ != nullptr) audit_->profile_activated(previous, ref_from(active_[target_index]));
  return true;
}

bool ProfileManager::rollback(ProfileTarget target) noexcept {
  const auto target_index = index(target);
  const ProfileRef previous = active_present_[target_index]
                                  ? ref_from(active_[target_index])
                                  : ProfileRef{};
  if (persistence_.rollback(target) != ProfileReadStatus::Found || !load(target)) return false;
  if (audit_ != nullptr) audit_->profile_activated(previous, ref_from(active_[target_index]));
  return true;
}

bool ProfileManager::reset(ProfileTarget target) noexcept {
  const auto target_index = index(target);
  if (!persistence_.reset(target)) return false;
  active_present_[target_index] = false;
  draft_present_[target_index] = false;
  active_[target_index] = EncodedProfile{};
  draft_[target_index] = EncodedProfile{};
  states_[target_index] = ProfileManagerState::Unloaded;
  sync_runtime_target(target);
  ++generation_;
  return true;
}

bool ProfileManager::export_active(ProfileTarget target,
                                   EncodedProfile& encoded) const noexcept {
  const auto target_index = index(target);
  if (!active_present_[target_index]) return false;
  encoded = active_[target_index];
  return true;
}

bool ProfileManager::snapshot(ProfileTarget target,
                              ActiveProfileSnapshot& snapshot) const noexcept {
  const auto target_index = index(target);
  snapshot = ActiveProfileSnapshot{};
  if (!active_present_[target_index]) return true;
  snapshot.present = true;
  snapshot.encoded = active_[target_index];
  snapshot.ref = ref_from(active_[target_index]);
  return true;
}

ProfileManagerState ProfileManager::state(ProfileTarget target) const noexcept {
  return states_[index(target)];
}

void ProfileManager::bind_runtime_profiles(TargetProfileSet& profiles) noexcept {
  runtime_profiles_ = &profiles;
  profiles = TargetProfileSet{};
  for (uint32_t target = 1U; target < kTargetCount; ++target) {
    sync_runtime_target(static_cast<ProfileTarget>(target));
  }
}

void ProfileManager::sync_runtime_target(ProfileTarget target) noexcept {
  if (runtime_profiles_ == nullptr) return;
  const auto target_index = index(target);
  const bool present = active_present_[target_index];
  switch (target) {
    case ProfileTarget::Hx711Organic:
    case ProfileTarget::Hx711Inorganic:
    case ProfileTarget::Hx711B3: {
      const std::size_t slot = static_cast<std::size_t>(target) - 1U;
      runtime_profiles_->hx711[slot] = nullptr;
      if (present && codec_.decode(active_[target_index], hx711_runtime_[slot]))
        runtime_profiles_->hx711[slot] = &hx711_runtime_[slot];
      break;
    }
    case ProfileTarget::UltrasonicOrganic:
    case ProfileTarget::UltrasonicInorganic:
    case ProfileTarget::UltrasonicB3: {
      const std::size_t slot = static_cast<std::size_t>(target) - 4U;
      runtime_profiles_->ultrasonic[slot] = nullptr;
      if (present && codec_.decode(active_[target_index], ultrasonic_runtime_[slot]))
        runtime_profiles_->ultrasonic[slot] = &ultrasonic_runtime_[slot];
      break;
    }
    case ProfileTarget::Mq4:
    case ProfileTarget::Mq135:
    case ProfileTarget::Mq7:
    case ProfileTarget::Mq8: {
      const std::size_t slot = static_cast<std::size_t>(target) - 7U;
      runtime_profiles_->mq_adc[slot] = nullptr;
      if (present && codec_.decode(active_[target_index], mq_runtime_[slot]))
        runtime_profiles_->mq_adc[slot] = &mq_runtime_[slot];
      if (target == ProfileTarget::Mq7) {
        runtime_profiles_->heater = nullptr;
        if (present && codec_.decode(active_[target_index], heater_runtime_))
          runtime_profiles_->heater = &heater_runtime_;
      }
      break;
    }
    case ProfileTarget::Ds18b20B3:
      runtime_profiles_->ds18b20 = nullptr;
      if (present && codec_.decode(active_[target_index], ds_runtime_))
        runtime_profiles_->ds18b20 = &ds_runtime_;
      break;
    case ProfileTarget::Sht31_44:
    case ProfileTarget::Sht31_45: {
      const std::size_t slot = target == ProfileTarget::Sht31_44 ? 0U : 1U;
      runtime_profiles_->sht31[slot] = nullptr;
      if (present && codec_.decode(active_[target_index], sht_runtime_[slot]))
        runtime_profiles_->sht31[slot] = &sht_runtime_[slot];
      break;
    }
    case ProfileTarget::Unknown: break;
  }
}

}  // namespace sigap::acquisition
