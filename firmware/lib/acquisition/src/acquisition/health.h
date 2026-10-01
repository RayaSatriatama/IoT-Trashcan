#pragma once

#include <cstdint>

#include "acquisition/acquisition_types.h"

namespace sigap::acquisition {

enum class SensorHealth : uint8_t { Offline, Online, Degraded, Failed };

struct SensorHealthState {
  uint64_t last_attempt_us{0};
  uint64_t last_ready_us{0};
  uint32_t consecutive_failures{0};
  uint64_t total_attempts{0};
  uint64_t total_valid_samples{0};
  uint32_t timeout_count{0};
  uint32_t crc_error_count{0};
  uint32_t disconnect_count{0};
  SampleQuality readiness{SampleQuality::WarmingUp};
  SensorHealth health{SensorHealth::Offline};

  static constexpr bool is_hardware_failure(SampleQuality quality) noexcept {
    switch (quality) {
      case SampleQuality::Timeout:
      case SampleQuality::Disconnected:
      case SampleQuality::CrcError:
        return true;
      default:
        return false;
    }
  }

  void observe(uint64_t now_us, SampleQuality quality) noexcept {
    last_attempt_us = now_us;
    ++total_attempts;
    readiness = quality;
    if (quality == SampleQuality::Ready) {
      last_ready_us = now_us;
      ++total_valid_samples;
      consecutive_failures = 0;
      health = SensorHealth::Online;
      return;
    }

    // Operational readiness is separate from communication health.  A sensor
    // can be physically online while its profile is missing, a heater is in a
    // non-detect phase, or a sample is still warming up/calibrating.
    if (!is_hardware_failure(quality)) {
      consecutive_failures = 0;
      health = SensorHealth::Online;
      return;
    }

    ++consecutive_failures;
    if (quality == SampleQuality::Timeout) ++timeout_count;
    if (quality == SampleQuality::CrcError) ++crc_error_count;
    if (quality == SampleQuality::Disconnected) ++disconnect_count;
    health = consecutive_failures >= 3U ? SensorHealth::Failed : SensorHealth::Degraded;
  }
};

}  // namespace sigap::acquisition
