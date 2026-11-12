#pragma once
#include "test_platform.h"
class String { std::string value_; public: String()=default;String(const char* s):value_(s?s:""){} String(std::string s):value_(std::move(s)){} const char* c_str()const{return value_.c_str();} size_t length()const{return value_.size();} };
inline uint32_t millis(){return test_platform::now_us/1000;}
inline uint32_t micros(){return test_platform::now_us;}
inline void delayMicroseconds(uint32_t){}
inline void delay(uint32_t ms){test_platform::now_us+=static_cast<uint64_t>(ms)*1000;}
inline constexpr int INPUT=0,OUTPUT=1,INPUT_PULLUP=2,LOW=0,HIGH=1;
inline void pinMode(int,int){} inline void digitalWrite(int,int){} inline int digitalRead(int){return HIGH;} inline int analogRead(int){return 0;}
inline void noInterrupts(){} inline void interrupts(){}
struct SerialClass { explicit operator bool()const{return true;} void begin(int){} template<class...T>void printf(const char*,T...){} void println(const char*){} void print(const char*){} int available(){++test_platform::serial_polls;return test_platform::serial_input.size();} int read(){if(test_platform::serial_input.empty())return -1;auto c=test_platform::serial_input.front();test_platform::serial_input.erase(0,1);return c;} size_t write(const uint8_t* bytes,size_t n){test_platform::serial_output.append(reinterpret_cast<const char*>(bytes),n);return n;} size_t write(uint8_t){return 1;} int availableForWrite(){return static_cast<int>(test_platform::serial_writable);} };
inline SerialClass Serial;
struct EspClass { uint64_t getEfuseMac(){return 0x1234abcdef56ULL;} void restart(){++test_platform::restarts;} };inline EspClass ESP;
inline void configTime(long,int,const char*,const char*){}
