#pragma once
#include <cstddef>
#include <cstdint>
namespace sensor_io {
inline uint64_t now_us{0};
inline unsigned calls{0};
inline uint32_t hx_frame{0};
inline unsigned hx_cursor{0};
}
inline constexpr int INPUT=0, OUTPUT=1, INPUT_PULLUP=2, LOW=0, HIGH=1;
inline void pinMode(int, int) { ++sensor_io::calls; }
inline void digitalWrite(int, int) { ++sensor_io::calls; }
inline int digitalRead(int) {
  ++sensor_io::calls;
  if (sensor_io::hx_cursor++ == 0U) return LOW;
  const unsigned bit = 24U - ((sensor_io::hx_cursor - 1U) % 25U);
  return bit < 24U && (sensor_io::hx_frame & (1U << bit)) != 0U ? HIGH : LOW;
}
inline int analogRead(int) { ++sensor_io::calls; return 0; }
inline void delayMicroseconds(uint32_t) {}
inline void delay(uint32_t) {}
inline void noInterrupts() {}
inline void interrupts() {}
