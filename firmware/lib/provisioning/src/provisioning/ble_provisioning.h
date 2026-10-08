#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "provisioning_policy.h"
#include "wifi_manager.h"

#ifdef ARDUINO
class BLECharacteristic;
class BLEServer;
#endif

namespace sigap::provisioning {

inline constexpr char kProvisioningServiceUuid[] =
    "7d3f0001-7d3f-4a53-9a4b-534947415000";
inline constexpr char kProvisioningCommandUuid[] =
    "7d3f0002-7d3f-4a53-9a4b-534947415000";
inline constexpr char kProvisioningResponseUuid[] =
    "7d3f0003-7d3f-4a53-9a4b-534947415000";

enum class BleCommandKind : uint8_t { Status, Scan, Connect, Ping };

struct BleCommand final {
  BleCommandKind kind{BleCommandKind::Status};
  std::array<char, 33> ssid{};
  std::array<char, 65> password{};
  std::size_t ssid_size{0U};
  std::size_t password_size{0U};
};

enum class BleParseStatus : uint8_t {
  Accepted,
  Empty,
  Oversized,
  Invalid,
};

class BleCommandParser final {
 public:
  static BleParseStatus parse(std::string_view frame,
                              BleCommand& output) noexcept;
};

struct BleResponse final {
  std::array<char, 512> bytes{};
  std::size_t size{0U};
  bool notify{false};
};

class IBleProvisioningBackend {
 public:
  virtual ~IBleProvisioningBackend() = default;
  virtual WifiState wifi_state() const noexcept = 0;
  virtual std::string_view current_ssid() const noexcept = 0;
  virtual WifiScanStatus request_scan() noexcept = 0;
  virtual WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                                   std::size_t& count) noexcept = 0;
  virtual bool request_connect(std::string_view ssid,
                               std::string_view password) noexcept = 0;
  virtual void cancel() noexcept = 0;
};

class WifiProvisioningBackend final : public IBleProvisioningBackend {
 public:
  explicit WifiProvisioningBackend(WifiConnectionManager& manager) noexcept
      : manager_(manager) {}
  WifiState wifi_state() const noexcept override { return manager_.state(); }
  std::string_view current_ssid() const noexcept override {
    return manager_.current_ssid();
  }
  WifiScanStatus request_scan() noexcept override {
    return manager_.request_scan();
  }
  WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                           std::size_t& count) noexcept override {
    return manager_.poll_scan(output, capacity, count);
  }
  bool request_connect(std::string_view ssid,
                       std::string_view password) noexcept override {
    return manager_.request_candidate(ssid, password);
  }
  void cancel() noexcept override { manager_.cancel(); }

 private:
  WifiConnectionManager& manager_;
};

class BleProvisioningClock {
 public:
  virtual ~BleProvisioningClock() = default;
  virtual uint64_t now_us() const noexcept = 0;
};

class BleProvisioningSession final {
 public:
  BleProvisioningSession(IBleProvisioningBackend& backend,
                         BleProvisioningClock& clock,
                         ProvisioningPolicy policy = kDefaultProvisioningPolicy) noexcept
      : backend_(backend), clock_(clock), policy_(policy) {}

  void on_connect(uint64_t now_us) noexcept;
  void on_authenticated(bool authenticated) noexcept { authenticated_ = authenticated; }
  void on_disconnect() noexcept;
  bool connected() const noexcept { return connected_; }
  bool authenticated() const noexcept { return authenticated_; }
  void poll(BleResponse& response) noexcept;
  BleParseStatus receive(std::string_view frame, BleResponse& response) noexcept;

 private:
  void status_response(BleResponse& response) noexcept;
  void scan_response(WifiScanStatus status, BleResponse& response) noexcept;
  void set_response(BleResponse& response, const char* format, ...) noexcept;
  bool timed_out(uint64_t now_us) const noexcept;

  IBleProvisioningBackend& backend_;
  BleProvisioningClock& clock_;
  ProvisioningPolicy policy_{};
  bool connected_{false};
  bool authenticated_{false};
  uint64_t last_activity_us_{0U};
  bool scan_waiting_{false};
  std::array<WifiNetwork, 16> scan_results_{};
};

#ifdef ARDUINO
class ArduinoBleProvisioningService final {
 public:
  bool begin(BleProvisioningSession& session,
             std::string_view device_name) noexcept;
  void poll() noexcept;
  void handle_write(::BLECharacteristic* characteristic) noexcept;
  void handle_connect() noexcept;
  void handle_disconnect(::BLEServer* server) noexcept;

 private:
  BleProvisioningSession* session_{nullptr};
  void* server_{nullptr};
  void* command_{nullptr};
  void* response_{nullptr};
};
#endif

}  // namespace sigap::provisioning
