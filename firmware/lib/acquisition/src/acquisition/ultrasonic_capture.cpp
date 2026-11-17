#include "acquisition/ultrasonic_capture.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <cstdio>
#include <driver/mcpwm.h>
#include <esp_attr.h>
#include <esp_pm.h>
#include <esp_private/esp_clk.h>
#include <esp_timer.h>
#include <soc/mcpwm_struct.h>
#include <soc/mcpwm_reg.h>

namespace sigap::acquisition {
namespace {
DRAM_ATTR portMUX_TYPE capture_mux = portMUX_INITIALIZER_UNLOCKED;
UltrasonicCapture* measurement_owner = nullptr;  // Main task only; never an ISR.
bool initialized = false;
bool initialization_attempted = false;
int32_t initialization_error = ESP_ERR_INVALID_STATE;
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
esp_pm_lock_handle_t apb_lock = nullptr;
esp_pm_lock_handle_t sleep_lock = nullptr;
#endif
mcpwm_dev_t* device() noexcept { return &MCPWM0; }
uint64_t now_us() noexcept { return static_cast<uint64_t>(esp_timer_get_time()); }
void enable_channel(unsigned ch, bool enable) noexcept {
  device()->cap_chn_cfg[ch].capn_en = enable;
}
void enable_edge(unsigned ch, unsigned bit, bool enable) noexcept {
  auto& cfg = device()->cap_chn_cfg[ch];
  cfg.val = enable ? (cfg.val | (1U << bit)) : (cfg.val & ~(1U << bit));
}
uint32_t capture_value(unsigned ch) noexcept { return device()->cap_chn[ch].capn_value; }
void clear_capture_flags() noexcept { device()->int_clr.val = 7U << 27U; }
void disable_channels() noexcept {
  for (unsigned ch = 0; ch < 3; ++ch) enable_channel(ch, false);
}
}

UltrasonicCapture::~UltrasonicCapture() { cancel(); }

bool UltrasonicCapture::initialize(uint8_t echo_pin) noexcept {
  if (measurement_owner != nullptr && measurement_owner != this) {
    error_ = ESP_ERR_INVALID_STATE;
    return false;
  }
  // This application has no other MCPWM client. Only capture is enabled:
  // no PWM generator initialization or output GPIO mapping occurs.
  if (!initialized) {
    if (initialization_attempted) { error_ = initialization_error; return false; }
    initialization_attempted = true;
    for (unsigned ch = 0; ch < 3; ++ch) {
      // This SDK's newer enable_channel API installs an ISR even with a null
      // callback. Polling needs only the capture registers, not that handler.
      const auto edge = ch == 1U ? MCPWM_NEG_EDGE : MCPWM_POS_EDGE;
      error_ = mcpwm_capture_enable(MCPWM_UNIT_0,
          static_cast<mcpwm_capture_signal_t>(ch), edge, 0U);
      if (error_ != ESP_OK) { initialization_error = error_; return false; }
      device()->int_ena.val &= ~(1U << (27U + ch));
    }
    disable_channels();
    enable_edge(2U, 2U, false);
    enable_edge(2U, 1U, false);
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    error_ = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "sigap_echo", &apb_lock);
    if (error_ == ESP_OK)
      error_ = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sigap_echo_sleep", &sleep_lock);
    if (error_ != ESP_OK) { initialization_error = error_; return false; }
#endif
    initialized = true;
  }
  error_ = mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM_CAP_0, echo_pin);
  if (error_ == ESP_OK) error_ = mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM_CAP_1, echo_pin);
  owned_ = error_ == ESP_OK;
  return owned_;
}

bool UltrasonicCapture::trigger(uint8_t trigger_pin, uint8_t echo_pin,
                                uint32_t timeout_us) noexcept {
  if (active_ || timeout_us == 0U) return false;
  if (!initialize(echo_pin)) { emit_debug(); return false; }
  if (digitalRead(echo_pin) != LOW) { error_ = ESP_ERR_INVALID_STATE; emit_debug(); return false; }
  measurement_owner = this;
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  error_ = esp_pm_lock_acquire(apb_lock);
  if (error_ == ESP_OK) {
    error_ = esp_pm_lock_acquire(sleep_lock);
    if (error_ != ESP_OK) (void)esp_pm_lock_release(apb_lock);
  }
  if (error_ != ESP_OK) { measurement_owner = nullptr; emit_debug(); return false; }
  clock_locked_ = true;
#endif
  clock_hz_ = static_cast<uint32_t>(esp_clk_apb_freq());
  timeout_us_ = timeout_us;
  if (clock_hz_ == 0U || static_cast<uint64_t>(timeout_us) * clock_hz_ / 1000000U > UINT32_MAX) {
    error_ = ESP_ERR_NOT_SUPPORTED; cancel(); emit_debug(); return false;
  }
  // GPIO pulse and counter anchor are bounded register operations. Flash IPC
  // cannot pause this core between TRIG and the software-captured anchor.
  portENTER_CRITICAL(&capture_mux);
  clear_capture_flags();
  for (unsigned ch = 0; ch < 3; ++ch) enable_channel(ch, true);
  digitalWrite(trigger_pin, HIGH);
  delayMicroseconds(10U);
  digitalWrite(trigger_pin, LOW);
  device()->cap_chn_cfg[2].capn_sw = 1U;
  uint32_t flags = 0U;
  for (unsigned attempt = 0U; attempt < 32U; ++attempt) {
    flags = (device()->int_raw.val >> 27U) & 7U;
    if ((flags & 4U) != 0U) break;
  }
  anchor_ = capture_value(2U);
  trigger_us_ = now_us();
  portEXIT_CRITICAL(&capture_mux);
  active_ = (flags & 4U) != 0U;
  error_ = active_ ? ESP_OK : ESP_ERR_TIMEOUT;
  if (!active_) { cancel(); emit_debug(); }
  return active_;
}

UltrasonicEchoResult UltrasonicCapture::poll() noexcept {
  if (!active_) return {UltrasonicEchoStatus::Pending, 0U};
  portENTER_CRITICAL(&capture_mux);
  raw_ = (device()->int_raw.val >> 27U) & 7U;
  if ((raw_ & 3U) == 3U) {
    disable_channels();
    rise_ = capture_value(0U);
    fall_ = capture_value(1U);
  }
  portEXIT_CRITICAL(&capture_mux);
  if ((raw_ & 3U) != 3U) return {UltrasonicEchoStatus::Pending, 0U};
  ++frames_;
  const uint32_t rise_delta = rise_ - anchor_;
  const uint32_t fall_delta = fall_ - anchor_;
  const uint32_t width_ticks = fall_ - rise_;
  const uint64_t age = now_us() - trigger_us_;
  const uint64_t unambiguous_age = static_cast<uint64_t>(UINT32_MAX) * 1000000U / clock_hz_;
  const bool valid = rise_delta != 0U && fall_delta > rise_delta && width_ticks != 0U &&
      static_cast<uint64_t>(fall_delta) * 1000000U <= static_cast<uint64_t>(timeout_us_) * clock_hz_ &&
      age < unambiguous_age;
  UltrasonicEchoResult result{UltrasonicEchoStatus::OutOfRange, 0U};
  if (valid) {
    const uint64_t echo_us = (static_cast<uint64_t>(width_ticks) * 1000000U + clock_hz_ / 2U) / clock_hz_;
    const uint64_t elapsed_upper = (static_cast<uint64_t>(fall_delta) * 1000000U + clock_hz_ - 1U) / clock_hz_;
    if (echo_us != 0U && echo_us <= UINT32_MAX && elapsed_upper <= timeout_us_) {
      result = {UltrasonicEchoStatus::Ready, static_cast<uint32_t>(echo_us)};
      result.capture_time_valid = true;
      result.captured_us = trigger_us_ + elapsed_upper;
      ++ready_;
    }
  }
  if (result.status != UltrasonicEchoStatus::Ready) ++rejected_;
  cancel();
  emit_debug();
  return result;
}

void UltrasonicCapture::cancel() noexcept {
  if (measurement_owner == this) {
    disable_channels();
    clear_capture_flags();
    measurement_owner = nullptr;
  }
  active_ = false;
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  if (clock_locked_) {
    (void)esp_pm_lock_release(sleep_lock);
    (void)esp_pm_lock_release(apb_lock);
    clock_locked_ = false;
  }
#endif
}

void UltrasonicCapture::emit_debug() const noexcept {
  const uint64_t now = now_us();
  if (now - last_diagnostic_us_ < 1000000U) return;
  last_diagnostic_us_ = now;
  char line[256]{};
  const int n = std::snprintf(line, sizeof(line),
      "SIGAP_CAPTURE v=13 mode=2 unit=0 hz=%lu owned=%u err=%ld frames=%lu ready=%lu rejected=%lu timeout_us=%lu anchor=%lu rise=%lu fall=%lu raw=%lu\n",
      static_cast<unsigned long>(clock_hz_), owned_ ? 1U : 0U,
      static_cast<long>(error_), static_cast<unsigned long>(frames_),
      static_cast<unsigned long>(ready_), static_cast<unsigned long>(rejected_),
      static_cast<unsigned long>(timeout_us_), static_cast<unsigned long>(anchor_),
      static_cast<unsigned long>(rise_), static_cast<unsigned long>(fall_),
      static_cast<unsigned long>(raw_));
  if (n > 0 && static_cast<std::size_t>(n) < sizeof(line) && Serial.availableForWrite() >= n)
    (void)Serial.write(reinterpret_cast<const uint8_t*>(line), static_cast<std::size_t>(n));
}
}  // namespace sigap::acquisition
#endif
