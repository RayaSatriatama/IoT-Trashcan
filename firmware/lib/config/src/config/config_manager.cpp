#include "config_manager.h"

#include <limits>

namespace sigap::config {

ConfigManager::ConfigManager(IConfigSlotStore& store,
                             ValidationContext context) noexcept
    : store_(store), context_(context) {}

bool ConfigManager::load_valid_slot(ConfigSlot slot, AppConfig& config,
                                    ConfigIdentity& identity) const noexcept {
  EncodedConfig encoded{};
  if (!store_.read_slot(slot, encoded)) return false;

  ConfigHash hash{};
  AppConfig candidate{};
  if (!codec_.decode(encoded, candidate, hash)) return false;
  if (!validate_config(candidate, context_).ok) return false;

  config = candidate;
  identity.slot = slot;
  identity.schema_version = candidate.schema_version;
  identity.config_version = candidate.config_version;
  identity.hash = hash;
  identity.has_slot = true;
  return true;
}

ConfigBootResult ConfigManager::begin() noexcept {
  fallback_used_ = false;
  has_draft_ = false;
  has_active_slot_ = false;
  state_ = ConfigState::Booting;

  ConfigSlot preferred{};
  AppConfig preferred_config{};
  ConfigIdentity preferred_identity{};
  const bool pointer_present = store_.read_active_pointer(preferred);
  const bool preferred_valid =
      pointer_present &&
      load_valid_slot(preferred, preferred_config, preferred_identity);

  if (preferred_valid) {
    active_ = preferred_config;
    identity_ = preferred_identity;
    has_active_slot_ = true;
    state_ = ConfigState::Active;
    boot_result_ = ConfigBootResult::ActiveLoaded;
    return boot_result_;
  }

  AppConfig candidate_a{};
  AppConfig candidate_b{};
  ConfigIdentity identity_a{};
  ConfigIdentity identity_b{};
  const bool valid_a = load_valid_slot(ConfigSlot::A, candidate_a, identity_a);
  const bool valid_b = load_valid_slot(ConfigSlot::B, candidate_b, identity_b);

  if (valid_a || valid_b) {
    const bool choose_a = valid_a &&
                          (!valid_b || candidate_a.config_version >=
                                           candidate_b.config_version);
    active_ = choose_a ? candidate_a : candidate_b;
    identity_ = choose_a ? identity_a : identity_b;
    has_active_slot_ = true;
    fallback_used_ = true;
    state_ = ConfigState::Active;
    boot_result_ = ConfigBootResult::FallbackLoaded;
    return boot_result_;
  }

  if (!store_.is_provisioned()) {
    active_ = kFactoryDefaults;
    draft_ = kFactoryDefaults;
    has_draft_ = true;
    state_ = ConfigState::Draft;
    boot_result_ = ConfigBootResult::BootstrapDefaults;
    return boot_result_;
  }

  active_ = kFactoryDefaults;
  state_ = ConfigState::ConfigRequired;
  boot_result_ = ConfigBootResult::ConfigRequired;
  return boot_result_;
}

bool ConfigManager::start_draft() noexcept {
  if (state_ != ConfigState::Active && state_ != ConfigState::ConfigRequired &&
      state_ != ConfigState::Draft) {
    return false;
  }
  draft_ = active_;
  has_draft_ = true;
  state_ = ConfigState::Draft;
  return true;
}

bool ConfigManager::set_draft(const AppConfig& draft) noexcept {
  if (!has_draft_ || state_ == ConfigState::RebootRequired) return false;
  draft_ = draft;
  state_ = ConfigState::Draft;
  return true;
}

ConfigValidation ConfigManager::validate_draft() const noexcept {
  if (!has_draft_) {
    return ConfigValidation{false, ConfigValidationError::NoDraft};
  }
  return validate_config(draft_, context_);
}

bool ConfigManager::validate_draft_and_enter() noexcept {
  if (!validate_draft().ok) return false;
  state_ = ConfigState::Validated;
  return true;
}

bool ConfigManager::preview_draft() noexcept {
  if (state_ != ConfigState::Validated && state_ != ConfigState::Preview) {
    return false;
  }
  if (!validate_draft().ok) {
    state_ = ConfigState::Draft;
    return false;
  }
  state_ = ConfigState::Preview;
  return true;
}

bool ConfigManager::commit_draft() noexcept {
  if (state_ != ConfigState::Validated && state_ != ConfigState::Preview) {
    return false;
  }
  if (!validate_draft().ok ||
      active_.config_version == std::numeric_limits<uint64_t>::max()) {
    state_ = ConfigState::Draft;
    return false;
  }

  AppConfig candidate = draft_;
  candidate.config_version = active_.config_version + 1U;
  if (!codec_.encode(candidate, encoded_scratch_)) {
    state_ = ConfigState::Draft;
    return false;
  }

  const ConfigSlot next_slot =
      has_active_slot_ ? other_slot(identity_.slot) : ConfigSlot::A;
  if (!store_.write_slot(next_slot, encoded_scratch_) ||
      !store_.read_slot(next_slot, readback_scratch_)) {
    state_ = ConfigState::Draft;
    return false;
  }

  AppConfig readback_config{};
  ConfigHash readback_hash{};
  if (!codec_.decode(readback_scratch_, readback_config, readback_hash) ||
      !validate_config(readback_config, context_).ok ||
      readback_config.config_version != candidate.config_version ||
      readback_hash != encoded_scratch_.hash()) {
    state_ = ConfigState::Draft;
    return false;
  }

  if (!store_.mark_provisioned() || !store_.write_active_pointer(next_slot)) {
    state_ = ConfigState::Draft;
    return false;
  }

  draft_ = candidate;
  state_ = ConfigState::RebootRequired;
  return true;
}

}  // namespace sigap::config
