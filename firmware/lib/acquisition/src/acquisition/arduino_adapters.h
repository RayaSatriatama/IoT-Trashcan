#pragma once

#include "acquisition/clock.h"
#include "acquisition/heater.h"
#include "acquisition/sensors/mq_adc.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_timer.h>
#endif

namespace sigap::acquisition {

#ifdef ARDUINO
class ArduinoMonotonicClock final {
 public:
  // ESP-IDF exposes a signed 64-bit monotonic microsecond counter. This avoids
  // the 32-bit rollover of the Arduino microsecond counter in scheduling.
  uint64_t now_us() const noexcept {
    const int64_t value = esp_timer_get_time();
    return value < 0 ? 0U : static_cast<uint64_t>(value);
  }
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
