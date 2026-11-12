#pragma once
#include <cstdint>
#include <string_view>

class FakeSerial {
 public:
  void begin(unsigned) noexcept;
  void print(const char*) noexcept;
  void println(const char*) noexcept;
  void printf(const char* format, ...) noexcept;
};
extern FakeSerial Serial;
void delay(unsigned) noexcept;
class FakeEfuse {
 public: uint64_t getEfuseMac() const noexcept { return 0x1234; }
};
extern FakeEfuse ESP;
