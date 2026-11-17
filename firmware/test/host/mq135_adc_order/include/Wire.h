#pragma once

// Host stub of the Arduino I2C surface referenced by the target SHT31 adapter.
// The ordering test never opens the bus; the SHT31 adapter is not instantiated.

#include <cstddef>
#include <cstdint>

struct TwoWire {
  bool begin(int, int) { return true; }
  void setTimeOut(uint16_t) {}
  void beginTransmission(uint8_t) {}
  std::size_t write(uint8_t) { return 1; }
  int endTransmission(bool = true) { return 0; }
  std::size_t requestFrom(uint8_t, std::size_t, bool = true) { return 0; }
  int available() { return 0; }
  int read() { return -1; }
};

inline TwoWire Wire;
