#include "config_manager.h"

#include <algorithm>
#include <limits>

namespace sigap::config {

ConfigManager::ConfigManager(IConfigSlotStore& store,
                             ValidationContext context) noexcept
    : store_(store), context_(context) {}

ConfigManager::SlotLoadStatus ConfigManager::load_valid_slot(
    ConfigSlot slot, AppConfig& config, ConfigIdentity& identity) const noexcept {
  EncodedConfig encoded{};
  const StoreReadStatus read_status = store_.read_slot(slot, encoded);
  if (read_status == StoreReadStatus::NotFound) {
    return SlotLoadStatus::NotFound;
  }
  if (read_status == StoreReadStatus::Error) return SlotLoadStatus::Error;
  if (read_status == StoreReadStatus::Invalid) {
    return SlotLoadStatus::Invalid;
  }

  ConfigHash hash{};
  AppConfig candidate{};
  if (!codec_.decode(encoded, candidate, hash)) {
    return SlotLoadStatus::Invalid;
  }
  if (!validate_config(candidate, context_).ok) {
    return SlotLoadStatus::Invalid;
  }

  config = candidate;
  identity.slot = slot;
  identity.schema_version = candidate.schema_version;
  identity.config_version = candidate.config_version;
  identity.hash = hash;
  identity.has_slot = true;
  return SlotLoadStatus::Valid;
}

ConfigBootResult ConfigManager::enter_storage_error() noexcept {
  active_ = kFactoryDefaults;
  draft_ = kFactoryDefaults;
  identity_ = ConfigIdentity{};
  has_draft_ = false;
  has_active_slot_ = false;
  state_ = ConfigState::StorageError;
  boot_result_ = ConfigBootResult::StorageError;
  return boot_result_;
}

ConfigBootResult ConfigManager::begin() noexcept {
  fallback_used_ = false;
  has_draft_ = false;
  has_active_slot_ = false;
  has_activation_metadata_ = false;
  last_committed_version_ = 0;
  activation_metadata_ = ActivationMetadata{};
  state_ = ConfigState::Booting;

  const StoreReadStatus metadata_status =
      store_.read_activation_metadata(activation_metadata_);
  if (metadata_status == StoreReadStatus::Error ||
      metadata_status == StoreReadStatus::Invalid) {
    return enter_storage_error();
  }
  has_activation_metadata_ = metadata_status == StoreReadStatus::Found;
  if (has_activation_metadata_) {
    last_committed_version_ = activation_metadata_.active_version;

    AppConfig candidate{};
    ConfigIdentity identity{};
    const SlotLoadStatus active_status =
        load_valid_slot(activation_metadata_.active_slot, candidate, identity);
    const bool active_matches =
        active_status == SlotLoadStatus::Valid &&
        identity.config_version == activation_metadata_.active_version &&
        identity.hash == activation_metadata_.active_hash;
    if (active_matches) {
      active_ = candidate;
      identity_ = identity;
      has_active_slot_ = true;
      state_ = ConfigState::Active;
      boot_result_ = ConfigBootResult::ActiveLoaded;
      return boot_result_;
    }
    if (active_status == SlotLoadStatus::Error) return enter_storage_error();

    if (activation_metadata_.has_previous) {
      AppConfig previous{};
      ConfigIdentity previous_identity{};
      const SlotLoadStatus previous_status = load_valid_slot(
          activation_metadata_.previous_slot, previous, previous_identity);
      const bool previous_matches =
          previous_status == SlotLoadStatus::Valid &&
          previous_identity.config_version ==
              activation_metadata_.previous_version &&
          previous_identity.hash == activation_metadata_.previous_hash;
      if (previous_matches) {
        active_ = previous;
        identity_ = previous_identity;
        has_active_slot_ = true;
        fallback_used_ = true;
        state_ = ConfigState::Active;
        boot_result_ = ConfigBootResult::FallbackLoaded;
        return boot_result_;
      }
      if (previous_status == SlotLoadStatus::Error) {
        return enter_storage_error();
      }
    }
  }

  const ProvisioningStatus provisioning = store_.provisioning_status();
  if (provisioning == ProvisioningStatus::Error) {
    return enter_storage_error();
  }
  if (provisioning == ProvisioningStatus::Unprovisioned &&
      !has_activation_metadata_) {
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
  if (state_ == ConfigState::ConfigRequired && !has_activation_metadata_) {
    return false;
  }
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
  const uint64_t base_version =
      std::max(active_.config_version, last_committed_version_);
  if (!validate_draft().ok || base_version == std::numeric_limits<uint64_t>::max()) {
    state_ = ConfigState::Draft;
    return false;
  }

  AppConfig candidate = draft_;
  candidate.config_version = base_version + 1U;
  if (!codec_.encode(candidate, encoded_scratch_)) {
    state_ = ConfigState::Draft;
    return false;
  }

  const ConfigSlot next_slot =
      has_active_slot_ ? other_slot(identity_.slot) : ConfigSlot::A;
  if (!store_.write_slot(next_slot, encoded_scratch_) ||
      store_.read_slot(next_slot, readback_scratch_) != StoreReadStatus::Found) {
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

  if (!store_.mark_provisioned()) {
    state_ = ConfigState::Draft;
    return false;
  }

  ActivationMetadata next_metadata{};
  next_metadata.active_slot = next_slot;
  next_metadata.active_version = candidate.config_version;
  next_metadata.active_hash = encoded_scratch_.hash();
  if (has_active_slot_) {
    next_metadata.has_previous = true;
    next_metadata.previous_slot = identity_.slot;
    next_metadata.previous_version = identity_.config_version;
    next_metadata.previous_hash = identity_.hash;
  } else if (has_activation_metadata_) {
    next_metadata.has_previous = true;
    next_metadata.previous_slot = activation_metadata_.active_slot;
    next_metadata.previous_version = activation_metadata_.active_version;
    next_metadata.previous_hash = activation_metadata_.active_hash;
  }
  if (!store_.write_activation_metadata(next_metadata)) {
    state_ = ConfigState::Draft;
    return false;
  }

  draft_ = candidate;
  state_ = ConfigState::RebootRequired;
  return true;
}

}  // namespace sigap::config
