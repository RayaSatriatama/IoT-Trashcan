#pragma once
#include <cstddef>
#include "Arduino.h"
struct TwoWire {
  bool begin(int sda,int scl) { sensor_io::observe(sda);sensor_io::observe(scl);return true; }
  void setTimeOut(uint16_t) { ++sensor_io::calls; }
  void beginTransmission(uint8_t address) { ++sensor_io::calls;if(address!=0x44)sensor_io::forbidden=true; }
  std::size_t write(uint8_t) { ++sensor_io::calls;return 1; }
  int endTransmission(bool=true) { ++sensor_io::calls;return 1; }
  std::size_t requestFrom(uint8_t address,std::size_t,bool=true) { ++sensor_io::calls;if(address!=0x44)sensor_io::forbidden=true;return 0; }
  int available() { return 0; }
  int read() { return -1; }
};
inline TwoWire Wire;
