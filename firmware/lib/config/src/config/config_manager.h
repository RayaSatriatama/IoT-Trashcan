#pragma once

#include "config_codec.h"
#include "sigap/config/config_defaults.h"
#include "config_store.h"
#include "config_validator.h"

namespace sigap::config {

enum class ConfigField : uint8_t {
  Hx711PeriodMs,
  MqPeriodMs,
  UltrasonicPeriodMs,
  Ds18b20PeriodMs,
  Sht31PeriodMs,
  SummaryPeriodMs,
  RetryMinMs,
  RetryMaxMs,
  MaxRecordsPerBatch,
  FlashPendingQuota,
  SdPendingQuota,
};

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
  const ConfigIdentity& pending_identity() const noexcept { return pending_identity_; }
  const AppConfig& draft() const noexcept { return draft_; }
  bool has_draft() const noexcept { return has_draft_; }

  bool start_draft() noexcept;
  // Explicit operator-authorized bootstrap path for CONFIG_REQUIRED. This
  // never runs implicitly during boot and only creates a factory draft.
  bool start_factory_draft() noexcept;
  bool reset_draft() noexcept;
  bool cancel_draft() noexcept;
  bool set_draft(const AppConfig& draft) noexcept;
  bool set_draft_field(ConfigField field, uint64_t value) noexcept;
  ConfigValidation validate_draft() const noexcept;
  bool validate_draft_and_enter() noexcept;
  bool preview_draft() noexcept;
  bool commit_draft() noexcept;

 private:
  enum class SlotLoadStatus : uint8_t {
    Valid,
    NotFound,
    Invalid,
    Error,
  };

  SlotLoadStatus load_valid_slot(ConfigSlot slot, AppConfig& config,
                                 ConfigIdentity& identity) const noexcept;
  ConfigBootResult enter_storage_error() noexcept;

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
  bool has_activation_metadata_{false};
  uint64_t last_committed_version_{0};
  EncodedConfig encoded_scratch_{};
  EncodedConfig readback_scratch_{};
  ActivationMetadata activation_metadata_{};
  ConfigIdentity pending_identity_{};
};

}  // namespace sigap::config
