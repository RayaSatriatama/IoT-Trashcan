#pragma once
#include "Arduino.h"
struct TwoWire {
  bool begin(int, int) { ++sensor_io::calls; return true; }
  void setTimeOut(uint16_t) { ++sensor_io::calls; }
  void beginTransmission(uint8_t) { ++sensor_io::calls; }
  std::size_t write(uint8_t) { ++sensor_io::calls; return 1; }
  int endTransmission(bool = true) { ++sensor_io::calls; return 1; }
  std::size_t requestFrom(uint8_t, std::size_t, bool = true) { ++sensor_io::calls; return 0; }
  int available() { ++sensor_io::calls; return 0; }
  int read() { ++sensor_io::calls; return -1; }
};
inline TwoWire Wire;
