#pragma once
#ifdef ARDUINO
#include <cstdint>
#include "acquisition/sensors/ultrasonic.h"
namespace sigap::acquisition {
class UltrasonicCapture final {
 public:
  UltrasonicCapture() = default;
  UltrasonicCapture(const UltrasonicCapture&) = delete;
  UltrasonicCapture& operator=(const UltrasonicCapture&) = delete;
  ~UltrasonicCapture();
  bool trigger(uint8_t trigger_pin, uint8_t echo_pin, uint32_t timeout_us) noexcept;
  UltrasonicEchoResult poll() noexcept;
  void cancel() noexcept;
  uint64_t trigger_time_us() const noexcept { return trigger_us_; }
  uint32_t readout_grace_us() const noexcept { return 0U; }
  void emit_debug() const noexcept;
 private:
  bool initialize(uint8_t echo_pin) noexcept;
  bool owned_{false}, active_{false}, clock_locked_{false};
  uint32_t timeout_us_{30000U}, clock_hz_{0U};
  uint64_t trigger_us_{0U};
  uint32_t anchor_{0U}, rise_{0U}, fall_{0U}, raw_{0U};
  int32_t error_{0};
  uint32_t frames_{0U}, ready_{0U}, rejected_{0U};
  mutable uint64_t last_diagnostic_us_{0U};
};
}  // namespace sigap::acquisition
#endif
