#pragma once

#include <cstdint>

#include "sigap/build/build_config.h"
#include "config/config_types.h"

namespace sigap::config {

inline constexpr AppConfig kFactoryDefaults{
    build::kConfigSchemaVersion,
    0,
    SamplingConfig{100, 100, 1000, 1000, 2000},
    TelemetryConfig{15000, 128, 1000, 60000},
    StorageConfig{6ULL * 1024ULL * 1024ULL,
                  20ULL * 1024ULL * 1024ULL * 1024ULL},
};

}  // namespace sigap::config

namespace sigap::processing_policy {

// Processing cadence is an engineering execution policy, not telemetry batch
// cadence. Telemetry summary_period_ms remains owned by AppConfig.
inline constexpr uint64_t kProcessingPeriodUs{1'000'000U};

}  // namespace sigap::processing_policy

namespace sigap::runtime_debug_policy {

enum class LoopGapClass : uint8_t { Healthy = 0U, Late = 1U, Stalled = 2U };
inline constexpr uint64_t kLoopGapHealthyMaxUs{1'000'000U};
inline constexpr uint64_t kLoopGapLateMaxUs{5'000'000U};
constexpr bool valid_loop_gap_bounds(uint64_t healthy_max_us,
                                     uint64_t late_max_us) noexcept {
  return healthy_max_us > 0U && late_max_us > healthy_max_us;
}
constexpr LoopGapClass classify_loop_gap(uint64_t gap_us,
                                         uint64_t healthy_max_us = kLoopGapHealthyMaxUs,
                                         uint64_t late_max_us = kLoopGapLateMaxUs) noexcept {
  return gap_us <= healthy_max_us ? LoopGapClass::Healthy
       : gap_us <= late_max_us ? LoopGapClass::Late : LoopGapClass::Stalled;
}
static_assert(valid_loop_gap_bounds(kLoopGapHealthyMaxUs, kLoopGapLateMaxUs));

}  // namespace sigap::runtime_debug_policy
