#pragma once

// Host stub of the esp_adc_cal / soc ADC declarations the MQ-135 seam compiles
// against on the target (ESP-IDF 4.4.7 as shipped with arduino-esp32 2.0.17).
// The numeric values match that SDK, so the seam's static assertions are
// checked against the real constants.

#include <cstdint>

#include "Arduino.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

typedef enum { ADC_UNIT_1 = 1, ADC_UNIT_2 = 2 } adc_unit_t;

typedef enum {
  ADC_ATTEN_DB_0 = 0,
  ADC_ATTEN_DB_2_5 = 1,
  ADC_ATTEN_DB_6 = 2,
  ADC_ATTEN_DB_12 = 3,
} adc_atten_t;

typedef enum {
  ADC_WIDTH_BIT_9 = 0,
  ADC_WIDTH_BIT_10 = 1,
  ADC_WIDTH_BIT_11 = 2,
  ADC_WIDTH_BIT_12 = 3,
  ADC_WIDTH_BIT_MAX = 4,
} adc_bits_width_t;

inline constexpr int ADC_WIDTH_MAX{ADC_WIDTH_BIT_MAX};
inline constexpr int SOC_ADC_MAX_BITWIDTH{12};
inline constexpr int SOC_ADC_MAX_CHANNEL_NUM{10};

typedef enum {
  ESP_ADC_CAL_VAL_EFUSE_VREF = 0,
  ESP_ADC_CAL_VAL_EFUSE_TP = 1,
  ESP_ADC_CAL_VAL_DEFAULT_VREF = 2,
  ESP_ADC_CAL_VAL_EFUSE_TP_FIT = 3,
  ESP_ADC_CAL_VAL_MAX,
  ESP_ADC_CAL_VAL_NOT_SUPPORTED = ESP_ADC_CAL_VAL_MAX,
} esp_adc_cal_value_t;

struct esp_adc_cal_characteristics_t {
  adc_unit_t adc_num{ADC_UNIT_1};
  adc_atten_t atten{ADC_ATTEN_DB_0};
  adc_bits_width_t bit_width{ADC_WIDTH_BIT_9};
  uint32_t vref{0};
};

namespace adc_stub {

inline bool efuse_ok{true};
inline esp_adc_cal_value_t characterize_result{ESP_ADC_CAL_VAL_EFUSE_TP_FIT};
inline adc_unit_t last_characterized_unit{ADC_UNIT_1};
inline adc_atten_t last_characterized_atten{ADC_ATTEN_DB_0};
inline int characterize_calls{0};

// Same linear full-scale approximation the test uses to predict the value, so
// the assertion is about WHICH counts were converted, not about SDK accuracy.
inline uint32_t mv_for(uint32_t counts) {
  return static_cast<uint32_t>((counts * 3300U) / 4095U);
}

}  // namespace adc_stub

inline esp_err_t esp_adc_cal_check_efuse(esp_adc_cal_value_t) {
  return adc_stub::efuse_ok ? ESP_OK : ESP_FAIL;
}

inline esp_adc_cal_value_t esp_adc_cal_characterize(adc_unit_t unit, adc_atten_t atten,
                                                    adc_bits_width_t width, uint32_t vref,
                                                    esp_adc_cal_characteristics_t* chars) {
  ++adc_stub::characterize_calls;
  adc_stub::last_characterized_unit = unit;
  adc_stub::last_characterized_atten = atten;
  if (chars != nullptr) {
    chars->adc_num = unit;
    chars->atten = atten;
    chars->bit_width = width;
    chars->vref = vref;
  }
  return adc_stub::characterize_result;
}

inline uint32_t esp_adc_cal_raw_to_voltage(uint32_t counts,
                                           const esp_adc_cal_characteristics_t* chars) {
  adc_stub::calls.push_back({adc_stub::CallKind::Convert, 0,
                             chars == nullptr ? 0 : static_cast<int>(chars->atten),
                             static_cast<int>(counts)});
  if (chars == nullptr) return 0U;
  return adc_stub::mv_for(counts);
}
