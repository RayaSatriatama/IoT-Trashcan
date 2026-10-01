#pragma once

#include <cstdint>

#include "config_types.h"

namespace sigap::config {

struct ValidationContext {
  uint64_t usable_littlefs_bytes{0};
  uint64_t usable_sd_bytes{0};
};

enum class ConfigValidationError : uint8_t {
  None,
  SchemaUnsupported,
  InvalidPeriod,
  SummaryPeriod,
  RetryOrder,
  BatchLimit,
  FlashQuota,
  SdQuota,
  VersionOverflow,
  NoDraft,
};

struct ConfigValidation {
  bool ok{false};
  ConfigValidationError error{ConfigValidationError::None};
};

ConfigValidation validate_config(const AppConfig& config,
                                 const ValidationContext& context) noexcept;

const char* validation_error_name(ConfigValidationError error) noexcept;

}  // namespace sigap::config
