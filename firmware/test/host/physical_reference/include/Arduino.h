#pragma once
#include <cstdint>
namespace sensor_io {
inline uint64_t now_us{0};
inline unsigned calls{0};
inline bool forbidden{false};
inline void observe(int pin) {
  ++calls;
  if (pin != 5 && pin != 8 && pin != 9 && pin != 10 && pin != 11 && pin != 16 && pin != 17) forbidden = true;
}
}
inline constexpr int INPUT=0, OUTPUT=1, INPUT_PULLUP=2, LOW=0, HIGH=1;
inline void pinMode(int pin,int) { sensor_io::observe(pin); }
inline void digitalWrite(int pin,int) { sensor_io::observe(pin); }
inline int digitalRead(int pin) { sensor_io::observe(pin); return LOW; }
inline int analogRead(int pin) { sensor_io::observe(pin); return 731; }
inline void delayMicroseconds(uint32_t) {}
inline void delay(uint32_t) {}
inline void noInterrupts() {}
inline void interrupts() {}
