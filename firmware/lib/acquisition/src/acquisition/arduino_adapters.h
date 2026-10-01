#pragma once

#include "acquisition/heater.h"
#include "acquisition/sensors/mq_adc.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace sigap::acquisition {

#ifdef ARDUINO
class ArduinoMonotonicClock final {
 public:
  uint64_t now_us() const noexcept { return static_cast<uint64_t>(micros()); }
};

class ArduinoHeaterRail final : public IMq7HeaterRail {
 public:
  explicit ArduinoHeaterRail(uint8_t select_pin) noexcept : select_pin_(select_pin) {
    pinMode(select_pin_, OUTPUT);
  }
  bool select_high() noexcept override { digitalWrite(select_pin_, HIGH); return true; }
  bool select_low() noexcept override { digitalWrite(select_pin_, LOW); return true; }

 private:
  uint8_t select_pin_;
};

class ArduinoMqAdcReader final : public IMqAdcReader {
 public:
  explicit ArduinoMqAdcReader(uint8_t pin) noexcept : pin_(pin) {}
  MqAdcReadResult read() noexcept override {
    const int value = analogRead(pin_);
    if (value < 0) return MqAdcReadResult{false, true, 0, SampleReason::Disconnected};
    return MqAdcReadResult{true, false, static_cast<uint16_t>(value), SampleReason::DataReady};
  }

 private:
  uint8_t pin_;
};
#endif

}  // namespace sigap::acquisition
