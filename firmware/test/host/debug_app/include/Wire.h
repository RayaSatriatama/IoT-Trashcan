#pragma once
#include <cstdint>
struct TwoWire { bool begin(int,int){return true;} void setTimeOut(uint16_t){} void beginTransmission(uint8_t){} size_t write(uint8_t){return 1;} int endTransmission(bool=true){return 1;} size_t requestFrom(uint8_t,size_t,bool=true){return 0;} int available(){return 0;} int read(){return -1;} };inline TwoWire Wire;
