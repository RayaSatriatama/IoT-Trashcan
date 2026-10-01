#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sigap::config {

inline constexpr std::size_t kConfigHashBytes{32};

using ConfigHash = std::array<uint8_t, kConfigHashBytes>;

struct SamplingConfig {
  uint32_t hx711_period_ms{100};
  uint32_t mq_period_ms{100};
  uint32_t ultrasonic_period_ms{1000};
  uint32_t ds18b20_period_ms{1000};
  uint32_t sht31_period_ms{2000};
};

struct TelemetryConfig {
  uint32_t summary_period_ms{15000};
  uint16_t max_records_per_batch{128};
  uint32_t retry_min_ms{1000};
  uint32_t retry_max_ms{60000};
};

struct StorageConfig {
  uint64_t flash_pending_quota_bytes{6ULL * 1024ULL * 1024ULL};
  uint64_t sd_pending_quota_bytes{20ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct AppConfig {
  uint32_t schema_version{1};
  uint64_t config_version{0};
  SamplingConfig sampling{};
  TelemetryConfig telemetry{};
  StorageConfig storage{};
};

enum class ConfigSlot : uint8_t { A = 0, B = 1 };

constexpr ConfigSlot other_slot(ConfigSlot slot) {
  return slot == ConfigSlot::A ? ConfigSlot::B : ConfigSlot::A;
}

struct ConfigIdentity {
  ConfigSlot slot{ConfigSlot::A};
  uint32_t schema_version{0};
  uint64_t config_version{0};
  ConfigHash hash{};
  bool has_slot{false};
};

enum class ConfigState : uint8_t {
  Booting,
  Active,
  Draft,
  Validated,
  Preview,
  RebootRequired,
  ConfigRequired,
};

enum class ConfigBootResult : uint8_t {
  ActiveLoaded,
  FallbackLoaded,
  BootstrapDefaults,
  ConfigRequired,
  StorageError,
};

}  // namespace sigap::config
