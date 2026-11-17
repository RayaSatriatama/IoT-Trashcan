#pragma once

// Host stub of the installed arduino-esp32 2.0.17 ADC surface, limited to the
// calls the real MQ-135 target adapters make. It reproduces the single SDK
// behaviour the ordering contract depends on: analogRead() samples with
// whatever attenuation is in effect at the moment of the call, and
// analogSetPinAttenuation() changes that setting immediately
// (cores/esp32/esp32-hal-adc.c:92-113, :153-183).
//
// Only the SDK is stubbed. The adapters and the esp_adc_cal seam under test are
// the real firmware headers.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adc_stub {

enum class CallKind : uint8_t {
  ConfigureAttenuation = 0,
  Read = 1,
  ReadMilliVolts = 2,
  Convert = 3,
};

struct Call {
  CallKind kind{CallKind::Read};
  uint8_t pin{0};
  int attenuation{0};  // in effect for a Read, requested for a configuration
  int counts{0};       // returned by a Read, converted by a Convert
};

// Deliberately NOT the 11 dB SDK default: starting at the default would hide
// the ordering defect this test exists to catch.
inline constexpr int kStartAttenuationAdc6db{2};
inline constexpr std::size_t kMaxPins{64};

inline std::vector<Call> calls{};
inline int global_attenuation{kStartAttenuationAdc6db};
inline int pin_attenuation[kMaxPins] = {};
inline bool pin_attenuation_set[kMaxPins] = {};
inline int read_value{2700};

inline void reset(int start_attenuation = kStartAttenuationAdc6db, int counts = 2700) {
  calls.clear();
  global_attenuation = start_attenuation;
  for (std::size_t i = 0; i < kMaxPins; ++i) {
    pin_attenuation[i] = -1;
    pin_attenuation_set[i] = false;
  }
  read_value = counts;
}

inline int attenuation_in_effect(uint8_t pin) {
  return pin_attenuation_set[pin] ? pin_attenuation[pin] : global_attenuation;
}

inline int count(CallKind kind) {
  int total = 0;
  for (const Call& call : calls) {
    if (call.kind == kind) ++total;
  }
  return total;
}

inline const Call* first(CallKind kind) {
  for (const Call& call : calls) {
    if (call.kind == kind) return &call;
  }
  return nullptr;
}

}  // namespace adc_stub

typedef enum { ADC_0db, ADC_2_5db, ADC_6db, ADC_11db } adc_attenuation_t;

inline constexpr int INPUT{0};
inline constexpr int OUTPUT{1};
inline constexpr int INPUT_PULLUP{2};
inline constexpr int ANALOG{5};
inline constexpr int LOW{0};
inline constexpr int HIGH{1};

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return LOW; }
inline void delayMicroseconds(uint32_t) {}
inline void delay(uint32_t) {}
inline void noInterrupts() {}
inline void interrupts() {}
inline uint32_t millis() { return 0; }
inline uint32_t micros() { return 0; }

// ESP32-S3 ADC1 channels 0..9 are GPIO1..GPIO10 (MQ-135 is GPIO5 -> channel 4).
inline int8_t digitalPinToAnalogChannel(uint8_t pin) {
  if (pin >= 1U && pin <= 10U) return static_cast<int8_t>(pin - 1U);
  return -1;
}

inline void analogSetPinAttenuation(uint8_t pin, adc_attenuation_t attenuation) {
  if (pin >= adc_stub::kMaxPins) return;
  adc_stub::pin_attenuation[pin] = static_cast<int>(attenuation);
  adc_stub::pin_attenuation_set[pin] = true;
  adc_stub::calls.push_back({adc_stub::CallKind::ConfigureAttenuation, pin,
                             static_cast<int>(attenuation), 0});
}

inline void analogSetAttenuation(adc_attenuation_t attenuation) {
  adc_stub::global_attenuation = static_cast<int>(attenuation);
}

inline int analogRead(uint8_t pin) {
  adc_stub::calls.push_back({adc_stub::CallKind::Read, pin,
                             adc_stub::attenuation_in_effect(pin), adc_stub::read_value});
  return adc_stub::read_value;
}

inline uint32_t analogReadMilliVolts(uint8_t pin) {
  adc_stub::calls.push_back({adc_stub::CallKind::ReadMilliVolts, pin,
                             adc_stub::attenuation_in_effect(pin), adc_stub::read_value});
  return 0U;
}

// Minimal USB-CDC serial surface used by acquisition/usb_sensor_debug.h. The
// ordering test never emits debug output; it only needs the symbols to compile.
struct HostSerialStub {
  int availableForWrite() { return 0; }
  std::size_t write(const uint8_t*, std::size_t size) { return size; }
};

inline HostSerialStub Serial;
