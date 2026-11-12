#pragma once
#include <cstdint>
#include <string>
class String { public: String(const char* value=""):value_(value){} const char* c_str() const{return value_.c_str();} unsigned length() const{return value_.size();} private: std::string value_; };
uint32_t millis();
inline void delay(uint32_t) {}

