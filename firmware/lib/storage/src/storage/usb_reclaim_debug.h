#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_timer.h>
#include <cstdio>

namespace sigap::storage {

class UsbReclaimDebug final {
 public:
  UsbReclaimDebug() noexcept : start_(now()), last_(start_), operation_(++counter_) {}
  ~UsbReclaimDebug() noexcept { emit(); }
  void mark(unsigned stage) noexcept {
    const uint64_t current = now();
    if (stage_ < 10U) elapsed_[stage_] += current - last_;
    stage_ = stage;
    last_ = current;
  }
  void source(uint64_t bytes) noexcept { source_bytes_ = bytes; }
  void result(bool ok) noexcept { ok_ = ok ? 1U : 0U; }

 private:
  static uint64_t now() noexcept {
    return static_cast<uint64_t>(esp_timer_get_time());
  }
  void emit() noexcept {
    mark(10U);
    char line[240]{};
    const int n = snprintf(line, sizeof(line),
        "SIGAP_RECLAIM v=7 op=%lu us=%llu src=1 bytes=%llu ok=%u s1=%llu s2=%llu s3=%llu s4=%llu s5=%llu s6=%llu s7=%llu s8=%llu s9=%llu\n",
        static_cast<unsigned long>(operation_),
        static_cast<unsigned long long>(now() - start_),
        static_cast<unsigned long long>(source_bytes_), ok_,
        static_cast<unsigned long long>(elapsed_[1]),
        static_cast<unsigned long long>(elapsed_[2]),
        static_cast<unsigned long long>(elapsed_[3]),
        static_cast<unsigned long long>(elapsed_[4]),
        static_cast<unsigned long long>(elapsed_[5]),
        static_cast<unsigned long long>(elapsed_[6]),
        static_cast<unsigned long long>(elapsed_[7]),
        static_cast<unsigned long long>(elapsed_[8]),
        static_cast<unsigned long long>(elapsed_[9]));
    if (n > 0 && static_cast<size_t>(n) < sizeof(line) &&
        Serial.availableForWrite() >= n) Serial.write(
            reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
  }
  inline static uint32_t counter_{0U};
  uint64_t start_{0U};
  uint64_t last_{0U};
  uint64_t source_bytes_{0U};
  uint64_t elapsed_[11]{};
  uint32_t operation_{0U};
  unsigned stage_{0U};
  unsigned ok_{0U};
};

}  // namespace sigap::storage
#endif
