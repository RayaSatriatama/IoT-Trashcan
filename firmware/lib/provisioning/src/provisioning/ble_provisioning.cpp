#include "ble_provisioning.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <BLE2902.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <esp_timer.h>
#endif

namespace sigap::provisioning {
namespace {

constexpr std::size_t kMaxFrameBytes{192U};

bool equal_literal(std::string_view value, const char* literal) noexcept {
  return value == std::string_view(literal);
}

bool has_control(std::string_view value) noexcept {
  for (const char byte : value) {
    if (static_cast<unsigned char>(byte) < 0x20U) return true;
  }
  return false;
}

const char* state_name(WifiState state) noexcept {
  switch (state) {
    case WifiState::Unconfigured: return "UNCONFIGURED";
    case WifiState::Connecting: return "CONNECTING";
    case WifiState::Connected: return "CONNECTED";
    case WifiState::Backoff: return "BACKOFF";
    case WifiState::SetupRequired: return "SETUP_REQUIRED";
  }
  return "UNKNOWN";
}

}  // namespace

BleParseStatus BleCommandParser::parse(std::string_view frame,
                                       BleCommand& output) noexcept {
  output = {};
  while (!frame.empty() && (frame.back() == '\n' || frame.back() == '\r')) {
    frame.remove_suffix(1U);
  }
  while (!frame.empty() && (frame.front() == ' ' || frame.front() == '\t')) {
    frame.remove_prefix(1U);
  }
  if (frame.empty()) return BleParseStatus::Empty;
  if (frame.size() > kMaxFrameBytes) return BleParseStatus::Oversized;
  if (has_control(frame)) return BleParseStatus::Invalid;
  if (equal_literal(frame, "STATUS")) {
    output.kind = BleCommandKind::Status;
    return BleParseStatus::Accepted;
  }
  if (equal_literal(frame, "SCAN")) {
    output.kind = BleCommandKind::Scan;
    return BleParseStatus::Accepted;
  }
  if (equal_literal(frame, "PING")) {
    output.kind = BleCommandKind::Ping;
    return BleParseStatus::Accepted;
  }
  constexpr std::string_view prefix{"CONNECT "};
  if (frame.substr(0U, prefix.size()) != prefix) return BleParseStatus::Invalid;
  frame.remove_prefix(prefix.size());
  const auto separator = frame.find(' ');
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U >= frame.size()) {
    return BleParseStatus::Invalid;
  }
  const auto ssid = frame.substr(0U, separator);
  const auto password = frame.substr(separator + 1U);
  if (ssid.size() > 32U || password.size() > 64U ||
      ssid.empty() || password.empty()) {
    return BleParseStatus::Invalid;
  }
  std::copy(ssid.begin(), ssid.end(), output.ssid.begin());
  std::copy(password.begin(), password.end(), output.password.begin());
  output.ssid_size = ssid.size();
  output.password_size = password.size();
  output.ssid[output.ssid_size] = '\0';
  output.password[output.password_size] = '\0';
  output.kind = BleCommandKind::Connect;
  return BleParseStatus::Accepted;
}

void BleProvisioningSession::on_connect(uint64_t now_us) noexcept {
  connected_ = true;
  authenticated_ = false;
  last_activity_us_ = now_us;
  scan_waiting_ = false;
}

void BleProvisioningSession::on_disconnect() noexcept {
  connected_ = false;
  authenticated_ = false;
  scan_waiting_ = false;
  backend_.cancel();
}

void BleProvisioningSession::set_response(BleResponse& response,
                                           const char* format, ...) noexcept {
  response = {};
  va_list arguments;
  va_start(arguments, format);
  const int result = std::vsnprintf(response.bytes.data(), response.bytes.size(),
                                    format, arguments);
  va_end(arguments);
  if (result <= 0) {
    response = {};
    return;
  }
  response.size = std::min<std::size_t>(static_cast<std::size_t>(result),
                                        response.bytes.size() - 1U);
  response.notify = true;
}

void BleProvisioningSession::status_response(BleResponse& response) noexcept {
  const auto ssid = backend_.current_ssid();
  set_response(response, "OK STATUS state=%s ssid=%.*s", state_name(backend_.wifi_state()),
               static_cast<int>(std::min<std::size_t>(ssid.size(), 32U)), ssid.data());
}

void BleProvisioningSession::scan_response(WifiScanStatus status,
                                           BleResponse& response) noexcept {
  switch (status) {
    case WifiScanStatus::Pending:
      set_response(response, "OK SCAN PENDING");
      return;
    case WifiScanStatus::RateLimited:
      set_response(response, "ERR SCAN_RATE_LIMIT");
      return;
    case WifiScanStatus::Failed:
      set_response(response, "ERR SCAN_FAILED");
      return;
    case WifiScanStatus::Idle:
      set_response(response, "ERR SCAN_IDLE");
      return;
    case WifiScanStatus::Ready:
      set_response(response, "OK SCAN READY");
      return;
  }
}

void BleProvisioningSession::poll(BleResponse& response) noexcept {
  response = {};
  if (!connected_) return;
  const uint64_t now = clock_.now_us();
  if (timed_out(now)) {
    on_disconnect();
    set_response(response, "ERR SESSION_TIMEOUT");
    return;
  }
  if (!scan_waiting_) return;
  std::size_t count{0U};
  const auto status = backend_.poll_scan(scan_results_.data(), scan_results_.size(), count);
  if (status == WifiScanStatus::Pending) return;
  scan_waiting_ = false;
  if (status != WifiScanStatus::Ready) {
    scan_response(status, response);
    return;
  }
  response = {};
  std::size_t offset{0U};
  const int prefix = std::snprintf(response.bytes.data(), response.bytes.size(),
                                   "OK SCAN READY count=%u", static_cast<unsigned>(count));
  if (prefix <= 0) return;
  offset = static_cast<std::size_t>(prefix);
  for (std::size_t i = 0U; i < count && offset < response.bytes.size(); ++i) {
    const auto& item = scan_results_[i];
    const int written = std::snprintf(
        response.bytes.data() + offset, response.bytes.size() - offset,
        "\n%.*s,%ld,%u", 32, item.ssid.data(), static_cast<long>(item.rssi),
        item.secure ? 1U : 0U);
    if (written <= 0 || static_cast<std::size_t>(written) >= response.bytes.size() - offset) {
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  response.size = offset;
  response.notify = true;
}

BleParseStatus BleProvisioningSession::receive(std::string_view frame,
                                               BleResponse& response) noexcept {
  response = {};
  if (!connected_) {
    set_response(response, "ERR DISCONNECTED");
    return BleParseStatus::Invalid;
  }
  if (!authenticated_) {
    set_response(response, "ERR AUTH_REQUIRED");
    return BleParseStatus::Invalid;
  }
  BleCommand command{};
  const auto status = BleCommandParser::parse(frame, command);
  if (status != BleParseStatus::Accepted) {
    set_response(response, status == BleParseStatus::Oversized ? "ERR OVERSIZED"
                                                               : "ERR INVALID");
    return status;
  }
  last_activity_us_ = clock_.now_us();
  switch (command.kind) {
    case BleCommandKind::Status:
      status_response(response);
      break;
    case BleCommandKind::Ping:
      set_response(response, "OK PONG");
      break;
    case BleCommandKind::Scan:
      scan_waiting_ = backend_.request_scan() == WifiScanStatus::Pending;
      scan_response(scan_waiting_ ? WifiScanStatus::Pending : WifiScanStatus::Failed,
                    response);
      break;
    case BleCommandKind::Connect: {
      const bool accepted = backend_.request_connect(
          std::string_view(command.ssid.data(), command.ssid_size),
          std::string_view(command.password.data(), command.password_size));
      set_response(response, accepted ? "OK CONNECTING" : "ERR CONNECT_REJECTED");
      break;
    }
  }
  return BleParseStatus::Accepted;
}

bool BleProvisioningSession::timed_out(uint64_t now_us) const noexcept {
  return now_us >= last_activity_us_ &&
         now_us - last_activity_us_ >=
             static_cast<uint64_t>(policy_.session_timeout_ms) * 1000U;
}

#ifdef ARDUINO
namespace {
class CommandCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit CommandCallbacks(ArduinoBleProvisioningService& owner) noexcept
      : owner_(owner) {}
  void onWrite(BLECharacteristic* characteristic) override;

 private:
  ArduinoBleProvisioningService& owner_;
};

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit ServerCallbacks(ArduinoBleProvisioningService& owner) noexcept
      : owner_(owner) {}
  void onConnect(BLEServer*) override;
  void onDisconnect(BLEServer*) override;

 private:
  ArduinoBleProvisioningService& owner_;
};
}  // namespace

bool ArduinoBleProvisioningService::begin(BleProvisioningSession& session,
                                           std::string_view device_name) noexcept {
  if (device_name.empty() || device_name.size() > 31U) return false;
  char name[32]{};
  std::copy(device_name.begin(), device_name.end(), name);
  BLEDevice::init(name);
  // GATT reads/writes are encrypted.  This uses the ESP-IDF/Bluedroid
  // authenticated link security boundary rather than a custom crypto scheme.
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  auto* server = BLEDevice::createServer();
  if (server == nullptr) return false;
  auto* service = server->createService(kProvisioningServiceUuid);
  if (service == nullptr) return false;
  auto* command = service->createCharacteristic(
      kProvisioningCommandUuid,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  auto* response = service->createCharacteristic(
      kProvisioningResponseUuid,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  if (command == nullptr || response == nullptr) return false;
  command->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  response->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  response->addDescriptor(new BLE2902());
  session_ = &session;
  server_ = server;
  command_ = command;
  response_ = response;
  command->setCallbacks(new CommandCallbacks(*this));
  server->setCallbacks(new ServerCallbacks(*this));
  service->start();
  server->getAdvertising()->addServiceUUID(kProvisioningServiceUuid);
  server->startAdvertising();
  return true;
}

void ArduinoBleProvisioningService::poll() noexcept {
  if (session_ == nullptr || response_ == nullptr) return;
  BleResponse response{};
  session_->poll(response);
  if (!response.notify || response.size == 0U) return;
  auto* characteristic = static_cast<BLECharacteristic*>(response_);
  characteristic->setValue(reinterpret_cast<uint8_t*>(response.bytes.data()),
                           response.size);
  characteristic->notify();
}

void CommandCallbacks::onWrite(BLECharacteristic* characteristic) {
  owner_.handle_write(characteristic);
}

void ServerCallbacks::onConnect(BLEServer*) {
  owner_.handle_connect();
}

void ServerCallbacks::onDisconnect(BLEServer* server) {
  owner_.handle_disconnect(server);
}

void ArduinoBleProvisioningService::handle_write(
    ::BLECharacteristic* characteristic) noexcept {
  if (characteristic == nullptr || session_ == nullptr || response_ == nullptr) return;
  const std::string value = characteristic->getValue();
  BleResponse result{};
  session_->receive(std::string_view(value.data(), value.size()), result);
  if (!result.notify || result.size == 0U) return;
  auto* response_characteristic = static_cast<BLECharacteristic*>(response_);
  response_characteristic->setValue(
      reinterpret_cast<uint8_t*>(result.bytes.data()), result.size);
  response_characteristic->notify();
}

void ArduinoBleProvisioningService::handle_connect() noexcept {
  if (session_ != nullptr) {
    session_->on_connect(static_cast<uint64_t>(esp_timer_get_time()));
    // The characteristic itself requires an encrypted GATT link.  The
    // session is opened only after that link has authenticated.
    session_->on_authenticated(true);
  }
}

void ArduinoBleProvisioningService::handle_disconnect(::BLEServer* server) noexcept {
  if (session_ != nullptr) session_->on_disconnect();
  if (server != nullptr) server->startAdvertising();
}
#endif

}  // namespace sigap::provisioning
