#pragma once

#include <cstddef>
#include <cstdint>

#include "acquisition/runtime.h"
#include "acquisition/profiles.h"
#include "acquisition/target_runtime.h"

namespace sigap {

// Commissioning result for HX711 tare and ultrasonic empty distance.
struct CommissioningResult final {
  bool hx711_tare_captured{false};
  float hx711_tare_counts{0.0F};
  bool ultrasonic_empty_captured{false};
  float ultrasonic_empty_mm{0.0F};
  uint32_t hx711_samples_used{0U};
  uint32_t ultrasonic_samples_used{0U};
  bool timeout{false};
};

// Captures initial empty references for HX711 (tare) and ultrasonic (empty distance)
// within a bounded deadline after setup. Returns result with captured values or timeout.
// - Collects up to max_samples READY samples per sensor.
// - Computes mean of valid samples.
// - Does NOT produce fake zero on failure/timeout.
// - Does NOT overwrite existing valid user calibration.
// - Profile initialization (if pristine) is handled separately via ProfileManager.
CommissioningResult capture_initial_empty_references(
    acquisition::AcquisitionFoundation& acquisition,
    uint32_t max_samples = 8U,
    uint32_t deadline_ms = 5000U) noexcept;

// Default prototype configuration constants (TEST_ONLY assumptions, not measured calibration).
// These are explicitly configurable assumptions for prototype operation only.
struct PrototypeConfig final {
  // HX711 nominal gain assumption: counts per kg
  float hx711_counts_per_kg = 100000.0F;
  // Capacity assumption: 10 kg
  float capacity_kg = 10.0F;
  // Ultrasonic full distance: 20 mm
  float ultrasonic_full_distance_mm = 20.0F;
  // Sound speed assumption: 0.1715 mm/us (343 m/s at 20C)
  float sound_speed_mm_per_us = 0.1715F;
  // ADC reference assumption for MQ135: 3300 mV full scale, 4095 counts, divider=1
  float adc_reference_mv = 3300.0F;
  uint16_t adc_max_counts = 4095U;
  float adc_divider = 1.0F;
};

inline constexpr PrototypeConfig kPrototypeConfig{};

}  // namespace sigap