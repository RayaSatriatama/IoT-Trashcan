#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sigap::board {

struct BoardPins {
  uint8_t mq4_adc{4};
  uint8_t mq135_adc{5};
  uint8_t mq7_adc{6};
  uint8_t mq8_adc{7};
  uint8_t organic_hx711_dout{10};
  uint8_t organic_hx711_sck{11};
  uint8_t inorganic_hx711_dout{12};
  uint8_t inorganic_hx711_sck{13};
  uint8_t b3_hx711_dout{14};
  uint8_t b3_hx711_sck{15};
  uint8_t organic_ultrasonic_trig{16};
  uint8_t organic_ultrasonic_echo{17};
  uint8_t inorganic_ultrasonic_trig{18};
  uint8_t inorganic_ultrasonic_echo{21};
  uint8_t b3_ultrasonic_trig{47};
  uint8_t b3_ultrasonic_echo{42};
  uint8_t ds18b20_data{2};
  uint8_t mq7_heater_select{1};
  uint8_t sht31_sda{8};
  uint8_t sht31_scl{9};
};

inline constexpr BoardPins kN16R8Pins{};

constexpr std::array<uint8_t, 20> pin_values(const BoardPins& pins) {
  return {
      pins.mq4_adc,
      pins.mq135_adc,
      pins.mq7_adc,
      pins.mq8_adc,
      pins.organic_hx711_dout,
      pins.organic_hx711_sck,
      pins.inorganic_hx711_dout,
      pins.inorganic_hx711_sck,
      pins.b3_hx711_dout,
      pins.b3_hx711_sck,
      pins.organic_ultrasonic_trig,
      pins.organic_ultrasonic_echo,
      pins.inorganic_ultrasonic_trig,
      pins.inorganic_ultrasonic_echo,
      pins.b3_ultrasonic_trig,
      pins.b3_ultrasonic_echo,
      pins.ds18b20_data,
      pins.mq7_heater_select,
      pins.sht31_sda,
      pins.sht31_scl,
  };
}

constexpr bool pins_are_unique(const BoardPins& pins) {
  const auto values = pin_values(pins);
  for (std::size_t i = 0; i < values.size(); ++i) {
    for (std::size_t j = i + 1; j < values.size(); ++j) {
      if (values[i] == values[j]) {
        return false;
      }
    }
  }
  return true;
}

constexpr bool is_reserved_pin(uint8_t pin) {
  return pin == 0 || pin == 3 || pin == 19 || pin == 20 ||
         (pin >= 26 && pin <= 37) || pin == 38 || pin == 45 || pin == 46 ||
         pin == 48;
}

constexpr bool uses_reserved_pin(const BoardPins& pins) {
  // cppcheck-suppress useStlAlgorithm
  for (const uint8_t pin : pin_values(pins)) {
    if (is_reserved_pin(pin)) {
      return true;
    }
  }
  return false;
}

static_assert(pins_are_unique(kN16R8Pins), "SIGAP board pins must be unique");
static_assert(!uses_reserved_pin(kN16R8Pins),
              "SIGAP board pins must avoid reserved GPIOs");

}  // namespace sigap::board
