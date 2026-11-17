#pragma once

#ifdef ARDUINO
#include <cstdint>
#include <limits>
#include <sys/time.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include "acquisition/record.h"

namespace sigap {
class ApplicationUtcSource final : public acquisition::IUtcSource {
 public:
  bool now_ms(int64_t& epoch_ms) noexcept override {
    uint64_t anchor_us{0U};
    return snapshot(epoch_ms, anchor_us);
  }

  bool at_monotonic_us(uint64_t captured_us, int64_t& epoch_ms) noexcept override {
    uint64_t anchor_us{0U};
    if (!snapshot(epoch_ms, anchor_us) || captured_us > anchor_us) return false;
    const uint64_t elapsed_us = anchor_us - captured_us;
    const uint64_t elapsed_ms = elapsed_us / 1000U + (elapsed_us % 1000U != 0U ? 1U : 0U);
    if (elapsed_ms > static_cast<uint64_t>(epoch_ms)) return false;
    epoch_ms -= static_cast<int64_t>(elapsed_ms);
    return true;
  }

  bool ready() const noexcept { return synchronized_; }

 private:
  bool snapshot(int64_t& epoch_ms, uint64_t& anchor_us) noexcept {
    // The SDK consumes COMPLETED and returns RESET between synchronization cycles.
    if (!synchronized_ && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
      synchronized_ = true;
    if (!synchronized_) return false;
    const int64_t before = esp_timer_get_time();
    timeval wall{};
    if (gettimeofday(&wall, nullptr) != 0) return false;
    const int64_t after = esp_timer_get_time();
    if (before < 0 || after < before || after - before > 2000 ||
        wall.tv_sec < 1700000000LL || wall.tv_usec < 0 || wall.tv_usec >= 1000000 ||
        static_cast<uint64_t>(wall.tv_sec) >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 1000 - 1))
      return false;
    anchor_us = static_cast<uint64_t>(before + (after - before) / 2);
    epoch_ms = static_cast<int64_t>(wall.tv_sec) * 1000 + wall.tv_usec / 1000;
    return true;
  }
  bool synchronized_{false};
};
}  // namespace sigap
#endif
