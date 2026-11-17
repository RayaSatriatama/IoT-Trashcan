#pragma once

// Internal same-sample calibrated ADC voltage evidence for the MQ-135 channel.
//
// S1 scope: this structure is INTERNAL. It is carried next to the existing raw
// ADC counts and does not change the legacy converted.adc_mv value/encoding, the
// telemetry-v3 writer or its schema. It is not persisted, not activated and not
// interpreted as a gas measurement here.
//
// Rules enforced below, per the reviewed S1 contract:
//   - the calibration probe is the installed ESP32-S3 SDK seam
//     `esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP_FIT)`;
//   - a non-success return or a characterization that does not report
//     EFUSE_TP_FIT is "unavailable" with an explicit failure, NEVER 0 mV and
//     NEVER a substituted 3300 mV default;
//   - the voltage is derived from the SAME `analogRead()` sample, never from a
//     second `analogReadMilliVolts()` pass, so same_sample identity is real;
//   - the resolution and attenuation recorded are the effective values actually
//     used for this channel (12 bit, 11 dB / ADC_ATTEN_DB_12 default here).

#include <cmath>
#include <cstdint>

namespace sigap::acquisition {

enum class MqAdcCalibrationMethod : uint8_t {
  None = 0,
  // ESP-IDF esp_adc_cal, eFuse two-point values with the fitting curve
  // (ESP_ADC_CAL_VAL_EFUSE_TP_FIT). The only method available on ESP32-S3.
  EspAdcCalEfuseTpFit = 1,
};

enum class MqAdcCalibrationFailure : uint8_t {
  None = 0,
  NotAttempted,
  EfuseUnsupported,
  CharacterizeUnsupported,
  ConversionFailed,
  RawOutOfRange,
  ImplausibleVoltage,
};

// Explicit physical unit of pin_mv, so a voltage can never be read as counts,
// ohms, a ratio or a concentration.
enum class MqAdcVoltageUnit : uint8_t {
  None = 0,
  Millivolt = 1,
};

struct MqAdcCalibratedVoltage final {
  bool available{false};
  // True only when the mV value was derived from the same analogRead() sample
  // carried beside it (counts must match the raw ADC counts exactly).
  bool same_sample{false};
  MqAdcCalibrationMethod method{MqAdcCalibrationMethod::None};
  MqAdcCalibrationFailure failure{MqAdcCalibrationFailure::NotAttempted};
  MqAdcVoltageUnit unit{MqAdcVoltageUnit::None};
  // Effective settings used for this channel when the sample was taken.
  uint8_t resolution_bits{0};
  uint8_t attenuation_db{0};
  uint8_t adc_unit{0};
  uint8_t adc_channel{0};
  uint16_t counts{0};
  float pin_mv{0.0F};
};

// Result of the SDK calibration probe for one sample. Filled by the target
// adapter; host tests fill it directly, so the arithmetic is testable without
// linking the SDK.
struct MqAdcCalibrationProbe final {
  bool efuse_tp_fit_supported{false};
  bool characterized_tp_fit{false};
  bool converted{false};
  uint32_t pin_mv{0};
};

// Full-scale guard bounds for the documented ESP32-S3 ADC settings.
inline constexpr uint16_t kMqAdcMaxCounts12Bit{4095U};
// A calibrated pin voltage above the ADC input range is not a reading.
inline constexpr uint32_t kMqAdcMaxPlausiblePinMv{3300U};

// Pure, SDK-free decision step. Every failure mode yields an explicit
// unavailable record with zeroed mV - never 0 mV as if it were a measurement
// and never a substituted reference voltage.
inline MqAdcCalibratedVoltage mq_adc_calibrated_voltage(
    const MqAdcCalibrationProbe& probe, uint16_t counts, uint8_t resolution_bits,
    uint8_t attenuation_db, uint8_t adc_unit, uint8_t adc_channel) noexcept {
  MqAdcCalibratedVoltage evidence{};
  evidence.resolution_bits = resolution_bits;
  evidence.attenuation_db = attenuation_db;
  evidence.adc_unit = adc_unit;
  evidence.adc_channel = adc_channel;

  if (resolution_bits != 12U) {
    // Only the current effective 12-bit setting is a supported domain.
    evidence.failure = MqAdcCalibrationFailure::RawOutOfRange;
    return evidence;
  }
  if (counts == 0U || counts >= kMqAdcMaxCounts12Bit) {
    evidence.failure = MqAdcCalibrationFailure::RawOutOfRange;
    return evidence;
  }
  if (!probe.efuse_tp_fit_supported) {
    evidence.failure = MqAdcCalibrationFailure::EfuseUnsupported;
    return evidence;
  }
  if (!probe.characterized_tp_fit) {
    evidence.failure = MqAdcCalibrationFailure::CharacterizeUnsupported;
    return evidence;
  }
  if (!probe.converted) {
    evidence.failure = MqAdcCalibrationFailure::ConversionFailed;
    return evidence;
  }
  if (probe.pin_mv == 0U || probe.pin_mv > kMqAdcMaxPlausiblePinMv) {
    evidence.failure = MqAdcCalibrationFailure::ImplausibleVoltage;
    return evidence;
  }

  evidence.available = true;
  evidence.same_sample = true;
  evidence.method = MqAdcCalibrationMethod::EspAdcCalEfuseTpFit;
  evidence.failure = MqAdcCalibrationFailure::None;
  evidence.unit = MqAdcVoltageUnit::Millivolt;
  evidence.counts = counts;
  evidence.pin_mv = static_cast<float>(probe.pin_mv);
  return evidence;
}

// A carried record is only self-consistent when the mV value belongs to the
// same raw counts and the effective settings are the documented ones.
inline bool mq_adc_calibration_matches(const MqAdcCalibratedVoltage& evidence,
                                       uint16_t counts) noexcept {
  if (!evidence.available || !evidence.same_sample) return false;
  if (evidence.failure != MqAdcCalibrationFailure::None) return false;
  if (evidence.counts != counts || counts == 0U || counts >= kMqAdcMaxCounts12Bit) return false;
  if (evidence.resolution_bits != 12U || evidence.attenuation_db != 11U) return false;
  if (evidence.adc_unit != 1U || evidence.adc_channel != 4U) return false;
  if (evidence.method != MqAdcCalibrationMethod::EspAdcCalEfuseTpFit) return false;
  if (evidence.unit != MqAdcVoltageUnit::Millivolt) return false;
  if (!std::isfinite(evidence.pin_mv) || evidence.pin_mv <= 0.0F ||
      evidence.pin_mv > static_cast<float>(kMqAdcMaxPlausiblePinMv)) return false;
  return true;
}

}  // namespace sigap::acquisition
