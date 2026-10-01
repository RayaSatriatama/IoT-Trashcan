#pragma once

#include "config_codec.h"
#include "sigap/config/config_defaults.h"
#include "config_store.h"
#include "config_validator.h"

namespace sigap::config {

class ConfigManager final {
 public:
  ConfigManager(IConfigSlotStore& store, ValidationContext context) noexcept;

  ConfigBootResult begin() noexcept;

  ConfigState state() const noexcept { return state_; }
  ConfigBootResult boot_result() const noexcept { return boot_result_; }
  bool fallback_used() const noexcept { return fallback_used_; }
  bool runtime_ready() const noexcept { return state_ == ConfigState::Active; }

  const AppConfig& active() const noexcept { return active_; }
  const ConfigIdentity& identity() const noexcept { return identity_; }
  const AppConfig& draft() const noexcept { return draft_; }
  bool has_draft() const noexcept { return has_draft_; }

  bool start_draft() noexcept;
  bool set_draft(const AppConfig& draft) noexcept;
  ConfigValidation validate_draft() const noexcept;
  bool validate_draft_and_enter() noexcept;
  bool preview_draft() noexcept;
  bool commit_draft() noexcept;

 private:
  bool load_valid_slot(ConfigSlot slot, AppConfig& config,
                       ConfigIdentity& identity) const noexcept;

  IConfigSlotStore& store_;
  ValidationContext context_{};
  ConfigCodec codec_{};
  AppConfig active_{kFactoryDefaults};
  AppConfig draft_{kFactoryDefaults};
  ConfigIdentity identity_{};
  ConfigState state_{ConfigState::Booting};
  ConfigBootResult boot_result_{ConfigBootResult::StorageError};
  bool fallback_used_{false};
  bool has_draft_{false};
  bool has_active_slot_{false};
  EncodedConfig encoded_scratch_{};
  EncodedConfig readback_scratch_{};
};

}  // namespace sigap::config
