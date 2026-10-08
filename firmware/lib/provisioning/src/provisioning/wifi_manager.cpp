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
  has_saved_ = secrets_.load_for_network(saved_);
  if (!has_saved_) {
    state_ = WifiState::SetupRequired;
    return true;
  }
  saved_ssid_size_ = bounded_string_size(saved_.ssid.data(), kMaxSsidBytes);
  if (saved_ssid_size_ == 0U) {
    has_saved_ = false;
    state_ = WifiState::SetupRequired;
    return true;
  }
  if (!station_.begin_candidate(
          std::string_view(saved_.ssid.data(), saved_ssid_size_),
          std::string_view(saved_.password.data()))) {
    state_ = WifiState::Backoff;
    next_retry_us_ = clock_.now_us() + 1000000U;
    return true;
  }
  candidate_ = saved_;
  candidate_ssid_size_ = saved_ssid_size_;
  candidate_active_ = true;
  candidate_requires_persist_ = false;
  candidate_started_us_ = clock_.now_us();
  state_ = WifiState::Connecting;
  return true;
}

void WifiConnectionManager::poll() noexcept {
  const uint64_t now = clock_.now_us();
  switch (state_) {
    case WifiState::Connecting:
      if (station_.connected() && station_.has_ip()) {
        if (candidate_active_ && !candidate_requires_persist_) {
          clear_candidate();
          state_ = WifiState::Connected;
        } else if (candidate_active_ &&
            (candidate_.ssid[0] != '\0' || candidate_.password[0] != '\0') &&
            secrets_.set_wifi(candidate_.ssid.data(), candidate_.password.data())) {
          saved_ = candidate_;
          saved_ssid_size_ = candidate_ssid_size_;
          has_saved_ = true;
          clear_candidate();
          state_ = WifiState::Connected;
        } else {
          station_.disconnect();
          clear_candidate();
          state_ = has_saved_ ? WifiState::Backoff : WifiState::SetupRequired;
          next_retry_us_ = now + 1000000U;
        }
        return;
      }
      if (station_.failed() ||
          elapsed_ms(now, candidate_started_us_, policy_.wifi_connect_timeout_ms)) {
        station_.disconnect();
        clear_candidate();
        state_ = has_saved_ ? WifiState::Backoff : WifiState::SetupRequired;
        next_retry_us_ = now + 1000000U;
      }
      return;
    case WifiState::Backoff:
      if (now < next_retry_us_) return;
      if (!has_saved_) {
        state_ = WifiState::SetupRequired;
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
        state_ = WifiState::Connecting;
      } else {
        clear_candidate();
        next_retry_us_ = now + 1000000U;
      }
      return;
    case WifiState::Connected:
      if (!station_.connected() || !station_.has_ip()) {
        state_ = has_saved_ ? WifiState::Backoff : WifiState::SetupRequired;
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
    state_ = has_saved_ ? WifiState::Backoff : WifiState::SetupRequired;
    next_retry_us_ = clock_.now_us() + 1000000U;
    return false;
  }
  candidate_started_us_ = clock_.now_us();
  state_ = WifiState::Connecting;
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
    state_ = WifiState::Backoff;
    next_retry_us_ = clock_.now_us() + 1000000U;
    return false;
  }
  state_ = WifiState::Connecting;
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
  if (scan_pending_) return WifiScanStatus::Pending;
  if (scan_ever_requested_ &&
      !elapsed_ms(now, last_scan_us_, policy_.scan_cooldown_ms)) {
    return WifiScanStatus::RateLimited;
  }
  if (!station_.begin_scan()) return WifiScanStatus::Failed;
  scan_pending_ = true;
  scan_ever_requested_ = true;
  last_scan_us_ = now;
  return WifiScanStatus::Pending;
}

WifiScanStatus WifiConnectionManager::poll_scan(WifiNetwork* output,
                                                std::size_t capacity,
                                                std::size_t& count) noexcept {
  count = 0U;
  if (!scan_pending_) return WifiScanStatus::Idle;
  const auto status = station_.poll_scan(output, capacity, count);
  if (status != WifiScanStatus::Pending) scan_pending_ = false;
  return status;
}

void WifiConnectionManager::clear_candidate() noexcept {
  secure_zero(&candidate_, sizeof(candidate_));
  candidate_ssid_size_ = 0U;
  candidate_active_ = false;
  candidate_requires_persist_ = false;
}

void WifiConnectionManager::restore_saved_or_setup() noexcept {
  state_ = has_saved_ ? WifiState::Backoff : WifiState::SetupRequired;
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
  return WiFi.scanNetworks(true, true) == WIFI_SCAN_RUNNING;
}

WifiScanStatus ArduinoWifiStation::poll_scan(WifiNetwork* output,
                                             std::size_t capacity,
                                             std::size_t& count) noexcept {
  const int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return WifiScanStatus::Pending;
  if (result < 0) {
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
