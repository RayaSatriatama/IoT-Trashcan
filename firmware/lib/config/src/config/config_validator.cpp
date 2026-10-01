#include "config_validator.h"

#include <limits>

#include "sigap/build/build_config.h"

namespace sigap::config {

namespace {

constexpr uint32_t kMinSummaryPeriodMs{5000};
constexpr uint32_t kMaxSummaryPeriodMs{300000};
constexpr uint32_t kMinUltrasonicPeriodMs{60};

ConfigValidation invalid(ConfigValidationError error) noexcept {
  return ConfigValidation{false, error};
}

}  // namespace

ConfigValidation validate_config(const AppConfig& config,
                                 const ValidationContext& context) noexcept {
  if (config.schema_version != build::kConfigSchemaVersion) {
    return invalid(ConfigValidationError::SchemaUnsupported);
  }

  if (config.sampling.hx711_period_ms == 0 ||
      config.sampling.mq_period_ms == 0 ||
      config.sampling.ultrasonic_period_ms < kMinUltrasonicPeriodMs ||
      config.sampling.ds18b20_period_ms == 0 ||
      config.sampling.sht31_period_ms == 0) {
    return invalid(ConfigValidationError::InvalidPeriod);
  }

  if (config.telemetry.summary_period_ms < kMinSummaryPeriodMs ||
      config.telemetry.summary_period_ms > kMaxSummaryPeriodMs) {
    return invalid(ConfigValidationError::SummaryPeriod);
  }

  if (config.telemetry.retry_min_ms == 0 ||
      config.telemetry.retry_max_ms == 0 ||
      config.telemetry.retry_min_ms > config.telemetry.retry_max_ms) {
    return invalid(ConfigValidationError::RetryOrder);
  }

  if (config.telemetry.max_records_per_batch == 0 ||
      config.telemetry.max_records_per_batch > build::kMaxRecordsPerBatch) {
    return invalid(ConfigValidationError::BatchLimit);
  }

  if (context.usable_littlefs_bytes <= build::kLittlefsReserveBytes ||
      config.storage.flash_pending_quota_bytes == 0 ||
      config.storage.flash_pending_quota_bytes >
          context.usable_littlefs_bytes - build::kLittlefsReserveBytes) {
    return invalid(ConfigValidationError::FlashQuota);
  }

  if (config.storage.sd_pending_quota_bytes == 0 ||
      (context.usable_sd_bytes != 0 &&
       config.storage.sd_pending_quota_bytes > context.usable_sd_bytes)) {
    return invalid(ConfigValidationError::SdQuota);
  }

  if (config.config_version == std::numeric_limits<uint64_t>::max()) {
    return invalid(ConfigValidationError::VersionOverflow);
  }

  return ConfigValidation{true, ConfigValidationError::None};
}

const char* validation_error_name(ConfigValidationError error) noexcept {
  switch (error) {
    case ConfigValidationError::None:
      return "NONE";
    case ConfigValidationError::SchemaUnsupported:
      return "SCHEMA_UNSUPPORTED";
    case ConfigValidationError::InvalidPeriod:
      return "INVALID_PERIOD";
    case ConfigValidationError::SummaryPeriod:
      return "SUMMARY_PERIOD";
    case ConfigValidationError::RetryOrder:
      return "RETRY_ORDER";
    case ConfigValidationError::BatchLimit:
      return "BATCH_LIMIT";
    case ConfigValidationError::FlashQuota:
      return "FLASH_QUOTA";
    case ConfigValidationError::SdQuota:
      return "SD_QUOTA";
    case ConfigValidationError::VersionOverflow:
      return "VERSION_OVERFLOW";
    case ConfigValidationError::NoDraft:
      return "NO_DRAFT";
  }
  return "UNKNOWN";
}

}  // namespace sigap::config
