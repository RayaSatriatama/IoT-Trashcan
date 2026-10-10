#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "provisioning_policy.h"
#include "provisioning_secret_store.h"
#include "wifi_manager.h"

#ifdef ARDUINO
class NimBLECharacteristic;
class NimBLEServer;
class NimBLEConnInfo;
#endif

namespace sigap::provisioning {

inline constexpr char kProvisioningServiceUuid[] =
    "7d3f0001-7d3f-4a53-9a4b-534947415000";
inline constexpr char kProvisioningCommandUuid[] =
    "7d3f0002-7d3f-4a53-9a4b-534947415000";
inline constexpr char kProvisioningResponseUuid[] =
    "7d3f0003-7d3f-4a53-9a4b-534947415000";

enum class BleCommandKind : uint8_t {
  Status,
  DeviceIdentity,
  Scan,
  Connect,
  Ping,
  ListKnownWifi,
  RetryWifi,
  CalibrationCapabilities,
  CalibrationStatus,
  CalibrationEvidence,
  CalibrationActiveProfile,
  CalibrationStart,
  CalibrationCriteria,
  CalibrationCapture,
  CalibrationValidate,
  CalibrationPreview,
  CalibrationCommit,
  CalibrationCancel,
  CalibrationHistory,
  CalibrationRollback,
};

enum class PasskeyDisclosureStatus : uint8_t {
  Ready,
  AlreadyDisclosed,
  Unavailable,
};

// The only supported passkey handoff is a single, explicitly requested local
// disclosure.  The underlying NVS setup secret remains private.
class ILocalPasskeyHandoff {
 public:
  virtual ~ILocalPasskeyHandoff() = default;
  virtual PasskeyDisclosureStatus disclose_once(
      char* output, std::size_t capacity) noexcept = 0;
};

class LocalPasskeyHandoff final : public ILocalPasskeyHandoff {
 public:
  void configure(uint32_t passkey) noexcept {
    passkey_ = passkey;
    configured_ = passkey <= 999999U;
    disclosed_ = false;
  }

  PasskeyDisclosureStatus disclose_once(
      char* output, std::size_t capacity) noexcept override;

 private:
  uint32_t passkey_{0U};
  bool configured_{false};
  bool disclosed_{false};
};

struct BleCommand final {
  BleCommandKind kind{BleCommandKind::Status};
  std::array<char, 33> ssid{};
  std::array<char, 65> password{};
  std::size_t ssid_size{0U};
  std::size_t password_size{0U};
  std::array<char, 128> calibration_arguments{};
  std::size_t calibration_arguments_size{0U};
};

struct BleWireFrame final {
  uint32_t request_id{0U};
  uint16_t chunk_index{0U};
  uint16_t chunk_count{1U};
  std::array<char, 192> payload{};
  std::size_t payload_size{0U};
  bool framed{false};
};

enum class BleParseStatus : uint8_t {
  Accepted,
  Empty,
  Oversized,
  Invalid,
};

class BleCommandParser final {
 public:
  static BleParseStatus decode_frame(std::string_view frame,
                                     BleWireFrame& output) noexcept;
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
  virtual WifiScanStatus list_known_wifi(WifiNetwork* output,
                                          std::size_t capacity,
                                          std::size_t& count) noexcept = 0;
  virtual bool request_connect(std::string_view ssid,
                               std::string_view password) noexcept = 0;
  virtual bool retry_wifi() noexcept = 0;
  virtual void cancel() noexcept = 0;
};

// Calibration is deliberately exposed as a narrow transport seam.  The
// authoritative session/controller remains owned by the calibration module;
// this adapter only forwards a bounded, authenticated command and receives a
// bounded response.  A single controller can therefore arbitrate USB, BLE,
// and HTTPS callers without duplicating calibration state here.
class IBleCalibrationBackend {
 public:
  virtual ~IBleCalibrationBackend() = default;
  virtual bool execute(std::string_view command, char* response,
                       std::size_t capacity,
                       std::size_t& response_size) noexcept = 0;
  virtual void disconnect() noexcept = 0;
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
  WifiScanStatus list_known_wifi(WifiNetwork* output, std::size_t capacity,
                                 std::size_t& count) noexcept override {
    return manager_.list_known(output, capacity, count);
  }
  bool request_connect(std::string_view ssid,
                       std::string_view password) noexcept override {
    return manager_.request_candidate(ssid, password);
  }
  bool retry_wifi() noexcept override { return manager_.retry_saved(); }
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
  // The canonical device identity is injected by the application composition
  // from the same EFUSE identity used by telemetry and device-control.  The
  // identity is only disclosed after the encrypted/authenticated GATT link is
  // established.
  void set_device_id(uint64_t device_id) noexcept { device_id_ = device_id; }
  void set_calibration_backend(IBleCalibrationBackend* backend) noexcept {
    calibration_backend_ = backend;
  }
  void on_disconnect() noexcept;
  bool connected() const noexcept { return connected_; }
  bool authenticated() const noexcept { return authenticated_; }
  void poll(BleResponse& response) noexcept;
  BleParseStatus receive(std::string_view frame, BleResponse& response) noexcept;

 private:
  static constexpr std::size_t kMaxCommandBytes{192U};
  static constexpr std::size_t kMaxResponseBytes{512U};
  static constexpr std::size_t kMaxWirePacketBytes{20U};
  static constexpr std::size_t kMaxChunks{128U};

  void status_response(BleResponse& response) noexcept;
  void device_identity_response(BleResponse& response) noexcept;
  void scan_response(WifiScanStatus status, BleResponse& response) noexcept;
  void set_response(BleResponse& response, const char* format, ...) noexcept;
  void queue_response(std::string_view payload, uint32_t request_id) noexcept;
  void emit_response(BleResponse& response) noexcept;
  void reset_reassembly() noexcept;
  BleParseStatus dispatch(std::string_view payload, uint32_t request_id,
                          BleResponse& response) noexcept;
  bool timed_out(uint64_t now_us) const noexcept;
  bool calibration_command(BleCommandKind kind) const noexcept;

  IBleProvisioningBackend& backend_;
  BleProvisioningClock& clock_;
  ProvisioningPolicy policy_{};
  bool connected_{false};
  bool authenticated_{false};
  uint64_t device_id_{0U};
  uint64_t last_activity_us_{0U};
  bool scan_waiting_{false};
  std::array<WifiNetwork, 16> scan_results_{};
  uint32_t scan_request_id_{0U};
  std::array<char, kMaxCommandBytes> reassembly_{};
  std::size_t reassembly_size_{0U};
  uint32_t reassembly_request_id_{0U};
  uint32_t last_request_id_{0U};
  bool has_last_request_id_{false};
  uint16_t reassembly_next_chunk_{0U};
  uint16_t reassembly_chunk_count_{0U};
  bool reassembly_active_{false};
  std::array<char, kMaxResponseBytes> pending_response_{};
  std::size_t pending_response_size_{0U};
  std::size_t pending_response_offset_{0U};
  uint32_t pending_response_id_{0U};
  uint16_t pending_response_chunk_{0U};
  uint32_t response_request_id_{0U};
  IBleCalibrationBackend* calibration_backend_{nullptr};
  std::array<char, kMaxResponseBytes> calibration_response_{};
};

#ifdef ARDUINO
class ArduinoBleProvisioningService final : public ILocalPasskeyHandoff {
 public:
  bool begin(BleProvisioningSession& session,
             std::string_view device_name) noexcept;
  void poll() noexcept;
  void handle_write(::NimBLECharacteristic* characteristic,
                    bool authenticated) noexcept;
  void handle_connect() noexcept;
  void handle_authenticated(bool authenticated) noexcept;
  void handle_disconnect(::NimBLEServer* server) noexcept;
  uint32_t setup_passkey() const noexcept { return setup_passkey_; }
  PasskeyDisclosureStatus disclose_once(
      char* output, std::size_t capacity) noexcept override {
    return passkey_handoff_.disclose_once(output, capacity);
  }

 private:
  BleProvisioningSession* session_{nullptr};
  void* server_{nullptr};
  void* command_{nullptr};
  void* response_{nullptr};
  NvsProvisioningSecretStore secret_store_{};
  LocalPasskeyHandoff passkey_handoff_{};
  uint32_t setup_passkey_{0U};
};
#endif

}  // namespace sigap::provisioning
