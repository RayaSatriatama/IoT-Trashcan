#include "prototype_commissioning.h"

#include <algorithm>

#include "acquisition/arduino_adapters.h"
#include "acquisition/handoff.h"
#include "acquisition/target_runtime.h"
#include "sigap/build/build_config.h"

namespace sigap {

namespace {

// Helper to check if a sample is READY and valid for commissioning
bool is_commissioning_ready(const acquisition::SampleRecord& sample) noexcept {
  return sample.quality == acquisition::SampleQuality::Ready &&
         sample.reason == acquisition::SampleReason::DataReady;
}

}  // namespace

CommissioningResult capture_initial_empty_references(
    acquisition::AcquisitionFoundation& acquisition,
    uint32_t max_samples,
    uint32_t deadline_ms) noexcept {
  CommissioningResult result{};

  const uint64_t start_us = acquisition::ArduinoMonotonicClock{}.now_us();
  const uint64_t deadline_us = start_us + static_cast<uint64_t>(deadline_ms) * 1000ULL;

  // Buffers for collecting samples
  float hx711_sum = 0.0F;
  uint32_t hx711_count = 0U;
  float us_sum = 0.0F;
  uint32_t us_count = 0U;

  // Poll acquisition until deadline or max samples reached for both
  while (acquisition::ArduinoMonotonicClock{}.now_us() < deadline_us &&
         (hx711_count < max_samples || us_count < max_samples)) {
    acquisition::SampleRecord sample{};
    const auto status = acquisition.handoff().try_pop(sample);
    if (status != acquisition::HandoffStatus::Dequeued) {
      continue;
    }

    if (!is_commissioning_ready(sample)) {
      continue;
    }

    // HX711 organic (target 0)
    if (sample.header.sensor == acquisition::SensorId::Hx711Organic && hx711_count < max_samples) {
      // Raw counts from HX711 sample
      if (sample.raw.present) {
        hx711_sum += static_cast<float>(sample.raw.signed_counts);
        ++hx711_count;
      }
    }

    // Ultrasonic organic (target 1)
    if (sample.header.sensor == acquisition::SensorId::UltrasonicOrganic && us_count < max_samples) {
      if (sample.raw.present) {
        // Raw counts for ultrasonic is the echo time in microseconds
        // Convert to distance using sound speed assumption
        float echo_us = static_cast<float>(sample.raw.distance_us);
        float distance_mm = (echo_us * kPrototypeConfig.sound_speed_mm_per_us) / 2.0F;
        us_sum += distance_mm;
        ++us_count;
      }
    }
  }

  // Compute means if we got samples
  if (hx711_count > 0U) {
    result.hx711_tare_captured = true;
    result.hx711_tare_counts = hx711_sum / static_cast<float>(hx711_count);
    result.hx711_samples_used = hx711_count;
  }
  if (us_count > 0U) {
    result.ultrasonic_empty_captured = true;
    result.ultrasonic_empty_mm = us_sum / static_cast<float>(us_count);
    result.ultrasonic_samples_used = us_count;
  }

  result.timeout = (hx711_count == 0U && us_count == 0U);
  return result;
}

}  // namespace sigap