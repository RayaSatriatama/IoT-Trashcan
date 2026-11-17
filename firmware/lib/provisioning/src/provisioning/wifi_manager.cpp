#include "wifi_manager.h"

#include <algorithm>

#ifdef ARDUINO
#include <WiFi.h>
#endif

namespace sigap::provisioning {

namespace {
std::size_t bounded_string_size(const char* value, std::size_t max) noexcept {
  if (value == nullptr) return 0U;
  std::size_t size{0U};
  while (size < max && value[size] != '\0') ++size;
  return size;
}
}  // namespace

bool WifiConnectionManager::begin() noexcept {
  saved_ = {};
  candidate_ = {};
  candidate_active_ = false;
  candidate_requires_persist_ = false;
  saved_ssid_size_ = 0U;
  candidate_ssid_size_ = 0U;
  has_saved_ = secrets_.load_wifi_for_network(saved_);
  if (!has_saved_) {
    set_state(WifiState::SetupRequired, 0);
    return true;
  }
  saved_ssid_size_ = bounded_string_size(saved_.ssid.data(), kMaxSsidBytes);
  if (saved_ssid_size_ == 0U) {
    has_saved_ = false;
    set_state(WifiState::SetupRequired, 0);
    return true;
  }
  if (!station_.begin_candidate(
          std::string_view(saved_.ssid.data(), saved_ssid_size_),
          std::string_view(saved_.password.data()))) {
    set_state(WifiState::Backoff, 2);
    next_retry_us_ = clock_.now_us();
    return true;
  }
  candidate_ = saved_;
  candidate_ssid_size_ = saved_ssid_size_;
  candidate_active_ = true;
  candidate_requires_persist_ = false;
  candidate_started_us_ = clock_.now_us();
  set_state(WifiState::Connecting, 0);
  return true;
}

void WifiConnectionManager::poll() noexcept {
  const uint64_t now = clock_.now_us();
  switch (state_) {
    case WifiState::Connecting:
      if (station_.connected() && station_.has_ip()) {
        if (candidate_active_ && !candidate_requires_persist_) {
          clear_candidate();
          set_state(WifiState::Connected, 0);
        } else if (candidate_active_ &&
            (candidate_.ssid[0] != '\0' || candidate_.password[0] != '\0') &&
            secrets_.set_wifi(candidate_.ssid.data(), candidate_.password.data())) {
          saved_ = candidate_;
          saved_ssid_size_ = candidate_ssid_size_;
          has_saved_ = true;
          clear_candidate();
          set_state(WifiState::Connected, 0);
        } else {
          station_.disconnect();
          clear_candidate();
          set_state(has_saved_ ? WifiState::Backoff : WifiState::SetupRequired, 2);
          next_retry_us_ = now;
        }
        return;
      }
      if (station_.failed() ||
          elapsed_ms(now, candidate_started_us_, policy_.wifi_connect_timeout_ms)) {
        station_.disconnect();
        clear_candidate();
        set_state(has_saved_ ? WifiState::Backoff : WifiState::SetupRequired, 2);
        next_retry_us_ = now;
      }
      return;
    case WifiState::Backoff:
      // Legacy state name; no added delay, retry at the next poll.
      if (now < next_retry_us_) return;
      if (!has_saved_) {
        set_state(WifiState::SetupRequired, 0);
        return;
      }
      candidate_ = saved_;
      candidate_ssid_size_ = saved_ssid_size_;
      candidate_active_ = true;
      candidate_requires_persist_ = false;
      candidate_started_us_ = now;
      if (station_.begin_candidate(
              std::string_view(candidate_.ssid.data(), candidate_ssid_size_),
              std::string_view(candidate_.password.data()))) {
        set_state(WifiState::Connecting, 1);
      } else {
        clear_candidate();
        next_retry_us_ = now;
      }
      return;
    case WifiState::Connected:
      if (!station_.connected() || !station_.has_ip()) {
        set_state(has_saved_ ? WifiState::Backoff : WifiState::SetupRequired, 1);
        next_retry_us_ = now;
      }
      return;
    case WifiState::Unconfigured:
    case WifiState::SetupRequired:
      return;
  }
}

bool WifiConnectionManager::copy_candidate(std::string_view ssid,
                                           std::string_view password) noexcept {
  if (ssid.empty() || ssid.size() > kMaxSsidBytes ||
      password.size() > kMaxPasswordBytes) {
    return false;
  }
  candidate_ = {};
  std::copy(ssid.begin(), ssid.end(), candidate_.ssid.begin());
  std::copy(password.begin(), password.end(), candidate_.password.begin());
  candidate_.ssid[ssid.size()] = '\0';
  candidate_.password[password.size()] = '\0';
  candidate_ssid_size_ = ssid.size();
  candidate_active_ = true;
  candidate_requires_persist_ = true;
  return true;
}

bool WifiConnectionManager::request_candidate(std::string_view ssid,
                                              std::string_view password) noexcept {
  if (!copy_candidate(ssid, password)) return false;
  station_.disconnect();
  if (!station_.begin_candidate(ssid, password)) {
    clear_candidate();
    set_state(has_saved_ ? WifiState::Backoff : WifiState::SetupRequired, 2);
    next_retry_us_ = clock_.now_us();
    return false;
  }
  candidate_started_us_ = clock_.now_us();
  set_state(WifiState::Connecting, 0);
  return true;
}

bool WifiConnectionManager::retry_saved() noexcept {
  if (!has_saved_ || saved_ssid_size_ == 0U) return false;
  station_.disconnect();
  candidate_ = saved_;
  candidate_ssid_size_ = saved_ssid_size_;
  candidate_active_ = true;
  candidate_requires_persist_ = false;
  candidate_started_us_ = clock_.now_us();
  if (!station_.begin_candidate(
          std::string_view(candidate_.ssid.data(), candidate_ssid_size_),
          std::string_view(candidate_.password.data()))) {
    clear_candidate();
    set_state(WifiState::Backoff, 2);
    next_retry_us_ = clock_.now_us();
    return false;
  }
  set_state(WifiState::Connecting, 1);
  return true;
}

WifiScanStatus WifiConnectionManager::list_known(
    WifiNetwork* output, std::size_t capacity,
    std::size_t& count) const noexcept {
  count = 0U;
  if (output == nullptr || capacity == 0U) {
    return WifiScanStatus::Idle;
  }
  // An unprovisioned device has a valid empty known-network list. Returning
  // Ready keeps the BLE setup flow usable before the first credential exists.
  if (!has_saved_ || saved_ssid_size_ == 0U) return WifiScanStatus::Ready;
  std::copy_n(saved_.ssid.data(), saved_ssid_size_, output[0].ssid.data());
  output[0].ssid[saved_ssid_size_] = '\0';
  output[0].rssi = 0;
  output[0].secure = saved_.password[0] != '\0';
  count = 1U;
  return WifiScanStatus::Ready;
}

void WifiConnectionManager::cancel() noexcept {
  if (candidate_active_) station_.disconnect();
  clear_candidate();
  restore_saved_or_setup();
}

WifiScanStatus WifiConnectionManager::request_scan() noexcept {
  const uint64_t now = clock_.now_us();
  if (scan_pending_) {
    emit_wifi(21U, 0, elapsed_metric(now, scan_started_us_));
    return WifiScanStatus::Pending;
  }
  if (policy_.scan_cooldown_ms != 0U && scan_ever_requested_ &&
      !elapsed_ms(now, last_scan_us_, policy_.scan_cooldown_ms)) {
    const uint64_t elapsed = now >= last_scan_us_ ? now - last_scan_us_ : 0U;
    const uint64_t cooldown = static_cast<uint64_t>(policy_.scan_cooldown_ms) * 1000U;
    const uint64_t remaining = elapsed >= cooldown ? 0U : cooldown - elapsed;
    const uint64_t seconds = remaining / 1000000U + (remaining % 1000000U != 0U);
    emit_wifi(23U, static_cast<int32_t>(std::min<uint64_t>(seconds, INT32_MAX)), 0);
    return WifiScanStatus::RateLimited;
  }
  if (!station_.begin_scan()) {
    if (station_.scan_failure_observation() == ScanFailureObservation::Driver)
      emit_wifi(24U, 0, 0);
    return WifiScanStatus::Failed;
  }
  scan_pending_ = true;
  scan_ever_requested_ = true;
  last_scan_us_ = now;
  scan_started_us_ = now;
  if (scan_attempts_ < static_cast<uint32_t>(INT32_MAX)) ++scan_attempts_;
  if (scan_source_valid_)
    emit_wifi(20U, static_cast<int32_t>(active_scan_source_), static_cast<int32_t>(scan_attempts_));
  return WifiScanStatus::Pending;
}

WifiScanStatus WifiConnectionManager::request_scan(WifiScanSource source) noexcept {
  active_scan_source_ = source;
  scan_source_valid_ = true;
  const auto status = request_scan();
  scan_source_valid_ = false;
  return status;
}

WifiScanStatus WifiConnectionManager::poll_scan(WifiNetwork* output,
                                                std::size_t capacity,
                                                std::size_t& count) noexcept {
  count = 0U;
  if (!scan_pending_) return WifiScanStatus::Idle;
  const auto status = station_.poll_scan(output, capacity, count);
  if (status == WifiScanStatus::Ready) {
    emit_wifi(22U, static_cast<int32_t>(std::min<std::size_t>(count, INT32_MAX)),
              elapsed_metric(clock_.now_us(), scan_started_us_));
  } else if (status == WifiScanStatus::Failed &&
             station_.scan_failure_observation() == ScanFailureObservation::Driver) {
    emit_wifi(24U, 0, 0);
  }
  if (status != WifiScanStatus::Pending) scan_pending_ = false;
  return status;
}

void WifiConnectionManager::emit_wifi(uint16_t code, int32_t arg0,
                                      int32_t arg1) noexcept {
  if (debug_sink_ == nullptr) return;
  debug::EventInput event{};
  event.monotonic_us = clock_.now_us();
  event.module = debug::Module::Wifi;
  event.event_code = code;
  event.level = debug::Level::Info;
  event.arg0 = arg0;
  event.arg1 = arg1;
  (void)debug_sink_->emit(event);
}

int32_t WifiConnectionManager::elapsed_metric(uint64_t now,
                                               uint64_t since) const noexcept {
  const uint64_t elapsed = now >= since ? now - since : 0U;
  return static_cast<int32_t>(std::min<uint64_t>(elapsed, INT32_MAX));
}

void WifiConnectionManager::set_state(WifiState state, int32_t reason) noexcept {
  if (state_ == state) return;
  state_ = state;
  const int32_t coarse = state == WifiState::Connected ? 2 :
                         state == WifiState::Connecting ? 1 : 0;
  emit_wifi(25U, coarse, reason);
}

void WifiConnectionManager::clear_candidate() noexcept {
  secure_zero(&candidate_, sizeof(candidate_));
  candidate_ssid_size_ = 0U;
  candidate_active_ = false;
  candidate_requires_persist_ = false;
}

void WifiConnectionManager::restore_saved_or_setup() noexcept {
  clear_candidate();
  set_state(has_saved_ ? WifiState::Backoff : WifiState::SetupRequired, 0);
}

bool WifiConnectionManager::elapsed_ms(uint64_t now_us, uint64_t since_us,
                                       uint32_t duration_ms) const noexcept {
  return now_us >= since_us && now_us - since_us >=
                                   static_cast<uint64_t>(duration_ms) * 1000U;
}

#ifdef ARDUINO

bool ArduinoWifiStation::begin_candidate(std::string_view ssid,
                                         std::string_view password) noexcept {
  if (ssid.empty() || ssid.size() > 32U || password.size() > 64U) return false;
  char ssid_buffer[33]{};
  char password_buffer[65]{};
  std::copy(ssid.begin(), ssid.end(), ssid_buffer);
  std::copy(password.begin(), password.end(), password_buffer);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_buffer, password_buffer);
  return true;
}

bool ArduinoWifiStation::connected() const noexcept {
  return WiFi.status() == WL_CONNECTED;
}

bool ArduinoWifiStation::has_ip() const noexcept {
  return connected() && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool ArduinoWifiStation::failed() const noexcept {
  const auto status = WiFi.status();
  return status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST ||
         status == WL_NO_SSID_AVAIL;
}

void ArduinoWifiStation::disconnect() noexcept { WiFi.disconnect(false, false); }

bool ArduinoWifiStation::begin_scan() noexcept {
  scan_failure_ = ScanFailureObservation::Unknown;
  const bool accepted = WiFi.scanNetworks(true, true) == WIFI_SCAN_RUNNING;
  if (!accepted) scan_failure_ = ScanFailureObservation::Driver;
  return accepted;
}

WifiScanStatus ArduinoWifiStation::poll_scan(WifiNetwork* output,
                                             std::size_t capacity,
                                             std::size_t& count) noexcept {
  scan_failure_ = ScanFailureObservation::Unknown;
  const int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return WifiScanStatus::Pending;
  if (result < 0) {
    scan_failure_ = ScanFailureObservation::Driver;
    WiFi.scanDelete();
    return WifiScanStatus::Failed;
  }
  if (output == nullptr && result > 0) {
    WiFi.scanDelete();
    return WifiScanStatus::Failed;
  }
  const std::size_t total = std::min<std::size_t>(static_cast<std::size_t>(result), capacity);
  for (std::size_t i = 0U; i < total; ++i) {
    const String ssid = WiFi.SSID(static_cast<int>(i));
    const std::size_t size = std::min<std::size_t>(ssid.length(), 32U);
    std::copy_n(ssid.c_str(), size, output[i].ssid.data());
    output[i].ssid[size] = '\0';
    output[i].rssi = WiFi.RSSI(static_cast<int>(i));
    output[i].secure = WiFi.encryptionType(static_cast<int>(i)) != WIFI_AUTH_OPEN;
  }
  count = total;
  WiFi.scanDelete();
  return WifiScanStatus::Ready;
}

#endif

}  // namespace sigap::provisioning
