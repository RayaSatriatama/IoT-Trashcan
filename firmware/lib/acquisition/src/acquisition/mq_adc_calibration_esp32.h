#pragma once

// ESP32-S3 target seam for the internal same-sample calibrated ADC voltage
// evidence. Included only from the Arduino target adapters, never from native
// tests, and it never performs an ADC conversion of its own: the caller passes
// the counts produced by its single analogRead() call.
//
// Ordering contract: the caller must apply mq_adc_configure_attenuation_for_pin()
// BEFORE that single analogRead(). The converters below only annotate the
// sample they are handed and never touch the ADC configuration afterwards.
//
// Installed SDK facts this file depends on (verified against the selected
// framework, arduino-esp32 2.0.17 / ESP-IDF 4.4.7):
//   * cores/esp32/esp32-hal-adc.c uses __analogAttenuation = 3 (ADC_11db) and
//     __analogWidth = ADC_WIDTH_MAX - 1 as defaults, and returns the raw
//     12-bit ADC1 value because __analogReturnedWidth == SOC_ADC_MAX_BITWIDTH.
//   * esp_adc_cal on ESP32-S3 supports only the eFuse two-point + fitting
//     scheme, so the probe must be ESP_ADC_CAL_VAL_EFUSE_TP_FIT and the
//     characterization return value must be checked. esp_adc_cal_characterize
//     returns ESP_ADC_CAL_VAL_NOT_SUPPORTED and leaves the characteristics
//     zeroed when no calibration eFuse is burnt; esp_adc_cal_raw_to_voltage
//     then yields 0 mV. Arduino's analogReadMilliVolts() ignores that return
//     value, which is why it is not used here.
//   * ESP32-S3 ADC1 channels 0..9 are GPIO1..GPIO10; the MQ-135 channel is
//     GPIO5 -> ADC1 channel 4.

#if defined(ARDUINO)

#include <Arduino.h>
#include <esp_adc_cal.h>

#include "acquisition/mq_adc_calibration.h"

namespace sigap::acquisition {

// Effective ADC configuration this firmware relies on and never changes for
// other channels. The static assertions fail the build if a framework update
// changes the defaults the evidence claims.
inline constexpr uint8_t kMqAdcEffectiveResolutionBits{12U};
inline constexpr uint8_t kMqAdcEffectiveAttenuationIndex{3U};
// Arduino labels attenuation index 3 "ADC_11db"; ESP-IDF names the same setting
// ADC_ATTEN_DB_12. Both denote the same ~3.3x input range.
inline constexpr uint8_t kMqAdcEffectiveAttenuationDb{11U};

static_assert(ADC_WIDTH_MAX - 1 == ADC_WIDTH_BIT_12,
              "analogRead() would shift the raw ADC1 sample; evidence would be mis-scaled");
static_assert(SOC_ADC_MAX_BITWIDTH == 12, "MQ-135 evidence requires a 12-bit ADC");
static_assert(static_cast<int>(ADC_ATTEN_DB_12) == 3, "ADC_ATTEN_DB_12 must be index 3");
static_assert(static_cast<int>(ADC_11db) == 3, "ADC_11db must be index 3");

struct MqAdcChannelMap final {
  uint8_t unit{0U};     // 1 = ADC1, 2 = ADC2, 0 = unresolved
  uint8_t channel{0U};  // unit-local channel
  bool resolved{false};
};

inline MqAdcChannelMap mq_adc_channel_map(uint8_t pin) noexcept {
  const int8_t index = digitalPinToAnalogChannel(pin);
  if (index < 0) return MqAdcChannelMap{};
  if (index >= SOC_ADC_MAX_CHANNEL_NUM) {
    return MqAdcChannelMap{2U, static_cast<uint8_t>(index - SOC_ADC_MAX_CHANNEL_NUM), true};
  }
  return MqAdcChannelMap{1U, static_cast<uint8_t>(index), true};
}

// One characterization per enabled channel, cached for the boot. The cached
// characteristics are only ever used after both gates below succeeded.
struct MqAdcCalibrationState final {
  uint8_t pin{0xFFU};
  bool probed{false};
  bool efuse_tp_fit_supported{false};
  bool characterized{false};
  esp_adc_cal_characteristics_t chars{};
};

inline MqAdcCalibrationState& mq_adc_calibration_state() noexcept {
  static MqAdcCalibrationState state{};
  return state;
}

// Applies the explicit per-channel attenuation for the MQ-135 pin. This MUST
// run BEFORE the analogRead() whose counts the evidence annotates: the core
// default happens to be 11 dB today, but the guarantee has to come from this
// firmware, not from an SDK default that a later analogSetAttenuation() call or
// a framework update could change. Returns false when the pin is not an ADC pin
// (nothing is configured and the caller's sample cannot be characterized).
inline bool mq_adc_configure_attenuation_for_pin(uint8_t pin) noexcept {
  if (!mq_adc_channel_map(pin).resolved) return false;
  analogSetPinAttenuation(pin, ADC_11db);
  return true;
}

// Calibrated voltage for the SAME raw sample the caller already read. It never
// reads or reconfigures the ADC: the caller owns the single analogRead() and
// must have applied mq_adc_configure_attenuation_for_pin() before it.
inline MqAdcCalibratedVoltage mq_adc_calibrated_voltage_for_pin(uint8_t pin,
                                                               uint16_t counts) noexcept {
  const MqAdcChannelMap channel = mq_adc_channel_map(pin);
  MqAdcCalibrationProbe probe{};
  if (!channel.resolved) {
    return mq_adc_calibrated_voltage(probe, counts, kMqAdcEffectiveResolutionBits,
                                     kMqAdcEffectiveAttenuationDb, 0U, 0U);
  }

  MqAdcCalibrationState& state = mq_adc_calibration_state();
  if (state.pin != pin || !state.probed) {
    state = MqAdcCalibrationState{};
    state.pin = pin;
    state.probed = true;
    state.efuse_tp_fit_supported =
        esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP_FIT) == ESP_OK;
    if (state.efuse_tp_fit_supported) {
      // Zero-initialised and only adopted when the characterization reports the
      // eFuse two-point fitting scheme. DEFAULT_VREF is unused on ESP32-S3.
      esp_adc_cal_characteristics_t chars{};
      const esp_adc_cal_value_t value = esp_adc_cal_characterize(
          channel.unit == 1U ? ADC_UNIT_1 : ADC_UNIT_2, ADC_ATTEN_DB_12,
          ADC_WIDTH_BIT_12, 1100U, &chars);
      if (value == ESP_ADC_CAL_VAL_EFUSE_TP_FIT) {
        state.chars = chars;
        state.characterized = true;
      }
    }
  }

  probe.efuse_tp_fit_supported = state.efuse_tp_fit_supported;
  probe.characterized_tp_fit = state.characterized;
  if (state.characterized && counts > 0U && counts <= kMqAdcMaxCounts12Bit) {
    probe.pin_mv = esp_adc_cal_raw_to_voltage(counts, &state.chars);
    probe.converted = true;
  }
  return mq_adc_calibrated_voltage(probe, counts, kMqAdcEffectiveResolutionBits,
                                   kMqAdcEffectiveAttenuationDb, channel.unit,
                                   channel.channel);
}

}  // namespace sigap::acquisition

#endif  // defined(ARDUINO)
