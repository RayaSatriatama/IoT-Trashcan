#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "provisioning_policy.h"
#include "secret_store.h"
#include "debug/types.h"

namespace sigap::provisioning {

enum class WifiState : uint8_t {
  Unconfigured,
  Connecting,
  Connected,
  Backoff,
  SetupRequired,
};

struct WifiNetwork final {
  std::array<char, 33> ssid{};
  int32_t rssi{0};
  bool secure{false};
};

enum class WifiScanStatus : uint8_t { Idle, Pending, Ready, Failed, RateLimited };
enum class WifiScanSource : uint8_t { Operator = 0, Internal = 1 };
enum class ScanFailureObservation : uint8_t { Unknown, Driver };

class IWifiStation {
 public:
  virtual ~IWifiStation() = default;
  virtual bool begin_candidate(std::string_view ssid,
                               std::string_view password) noexcept = 0;
  virtual bool connected() const noexcept = 0;
  virtual bool has_ip() const noexcept = 0;
  virtual bool failed() const noexcept = 0;
  virtual void disconnect() noexcept = 0;
  virtual bool begin_scan() noexcept = 0;
  virtual ScanFailureObservation scan_failure_observation() const noexcept {
    return ScanFailureObservation::Unknown;
  }
  virtual WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                                   std::size_t& count) noexcept = 0;
};

class IProvisioningClock {
 public:
  virtual ~IProvisioningClock() = default;
  virtual uint64_t now_us() const noexcept = 0;
};

class WifiConnectionManager final {
 public:
  WifiConnectionManager(SecretStore& secrets, IWifiStation& station,
                        IProvisioningClock& clock,
                        ProvisioningPolicy policy = kDefaultProvisioningPolicy) noexcept
      : secrets_(secrets), station_(station), clock_(clock), policy_(policy) {}

  bool begin() noexcept;
  void poll() noexcept;
  WifiState state() const noexcept { return state_; }
  std::string_view current_ssid() const noexcept {
    return std::string_view(saved_.ssid.data(), saved_ssid_size_);
  }
  bool has_saved_credentials() const noexcept { return has_saved_; }

  // Candidate credentials stay in RAM until association and IP acquisition
  // succeed.  A failed candidate never overwrites the saved credential.
  bool request_candidate(std::string_view ssid, std::string_view password) noexcept;
  bool retry_saved() noexcept;
  void cancel() noexcept;
  WifiScanStatus list_known(WifiNetwork* output, std::size_t capacity,
                            std::size_t& count) const noexcept;
  WifiScanStatus request_scan() noexcept;
  WifiScanStatus request_scan(WifiScanSource source) noexcept;
  WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                           std::size_t& count) noexcept;
  void bind_debug_sink(debug::IEventSink* sink) noexcept { debug_sink_ = sink; }

 private:
  static constexpr std::size_t kMaxSsidBytes{32U};
  static constexpr std::size_t kMaxPasswordBytes{64U};

  bool copy_candidate(std::string_view ssid, std::string_view password) noexcept;
  void clear_candidate() noexcept;
  void restore_saved_or_setup() noexcept;
  void set_state(WifiState state, int32_t reason = 0) noexcept;
  void emit_wifi(uint16_t code, int32_t arg0, int32_t arg1) noexcept;
  int32_t elapsed_metric(uint64_t now, uint64_t since) const noexcept;
  bool elapsed_ms(uint64_t now_us, uint64_t since_us,
                  uint32_t duration_ms) const noexcept;

  SecretStore& secrets_;
  IWifiStation& station_;
  IProvisioningClock& clock_;
  ProvisioningPolicy policy_{};
  WifiState state_{WifiState::Unconfigured};
  NetworkSecrets saved_{};
  NetworkSecrets candidate_{};
  std::size_t saved_ssid_size_{0U};
  std::size_t candidate_ssid_size_{0U};
  bool has_saved_{false};
  bool candidate_active_{false};
  bool candidate_requires_persist_{false};
  uint64_t candidate_started_us_{0U};
  uint64_t next_retry_us_{0U};
  uint64_t last_scan_us_{0U};
  bool scan_ever_requested_{false};
  bool scan_pending_{false};
  debug::IEventSink* debug_sink_{nullptr};
  uint32_t scan_attempts_{0U};
  WifiScanSource active_scan_source_{WifiScanSource::Operator};
  bool scan_source_valid_{false};
  uint64_t scan_started_us_{0U};
};

#ifdef ARDUINO
class ArduinoWifiStation final : public IWifiStation {
 public:
  bool begin_candidate(std::string_view ssid,
                       std::string_view password) noexcept override;
  bool connected() const noexcept override;
  bool has_ip() const noexcept override;
  bool failed() const noexcept override;
  void disconnect() noexcept override;
  bool begin_scan() noexcept override;
  ScanFailureObservation scan_failure_observation() const noexcept override {
    return scan_failure_;
  }
  WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                           std::size_t& count) noexcept override;
 private:
  ScanFailureObservation scan_failure_{ScanFailureObservation::Unknown};
};
#endif

}  // namespace sigap::provisioning
