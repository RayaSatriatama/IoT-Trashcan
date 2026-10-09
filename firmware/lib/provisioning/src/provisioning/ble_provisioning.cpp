#include "ble_provisioning.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <NimBLECharacteristic.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <esp_timer.h>
#endif

namespace sigap::provisioning {
namespace {

constexpr std::size_t kMaxFrameBytes{192U};
constexpr std::size_t kMaxChunks{128U};
constexpr std::size_t kMaxWirePacketBytes{20U};

bool equal_literal(std::string_view value, const char* literal) noexcept {
  return value == std::string_view(literal);
}

bool has_control(std::string_view value) noexcept {
  for (const char byte : value) {
    if (static_cast<unsigned char>(byte) < 0x20U) return true;
  }
  return false;
}

bool parse_uint(std::string_view value, uint32_t maximum,
                uint32_t& output) noexcept {
  if (value.empty()) return false;
  uint32_t parsed{0U};
  for (const char byte : value) {
    if (byte < '0' || byte > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(byte - '0');
    if (parsed > (maximum - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  output = parsed;
  return true;
}

bool calibration_operation(std::string_view operation,
                           BleCommandKind& kind) noexcept {
  if (operation == "CAPABILITIES") kind = BleCommandKind::CalibrationCapabilities;
  else if (operation == "STATUS") kind = BleCommandKind::CalibrationStatus;
  else if (operation == "EVIDENCE") kind = BleCommandKind::CalibrationEvidence;
  else if (operation == "ACTIVE_PROFILE") kind = BleCommandKind::CalibrationActiveProfile;
  else if (operation == "START") kind = BleCommandKind::CalibrationStart;
  else if (operation == "CRITERIA") kind = BleCommandKind::CalibrationCriteria;
  else if (operation == "CAPTURE") kind = BleCommandKind::CalibrationCapture;
  else if (operation == "VALIDATE") kind = BleCommandKind::CalibrationValidate;
  else if (operation == "PREVIEW") kind = BleCommandKind::CalibrationPreview;
  else if (operation == "COMMIT") kind = BleCommandKind::CalibrationCommit;
  else if (operation == "CANCEL") kind = BleCommandKind::CalibrationCancel;
  else if (operation == "HISTORY") kind = BleCommandKind::CalibrationHistory;
  else if (operation == "ROLLBACK") kind = BleCommandKind::CalibrationRollback;
  else return false;
  return true;
}

std::size_t next_field(std::string_view value, std::size_t start,
                       std::string_view& field) noexcept {
  if (start > value.size()) return std::string_view::npos;
  const auto end = value.find('|', start);
  if (end == std::string_view::npos) {
    field = value.substr(start);
    return value.size();
  }
  field = value.substr(start, end - start);
  return end + 1U;
}

std::size_t response_prefix_size(uint32_t request_id, uint16_t index,
                                 uint16_t count) noexcept {
  char prefix[48]{};
  const int result = std::snprintf(
      prefix, sizeof(prefix), "R1|%lu|%u|%u|",
      static_cast<unsigned long>(request_id), static_cast<unsigned>(index),
      static_cast<unsigned>(count));
  return result > 0 ? static_cast<std::size_t>(result) : sizeof(prefix);
}

std::size_t response_chunk_count(std::size_t payload_size,
                                 uint32_t request_id) noexcept {
  for (std::size_t count = 1U; count <= kMaxChunks; ++count) {
    std::size_t offset{0U};
    bool valid{true};
    for (std::size_t index = 0U; index < count && offset < payload_size;
         ++index) {
      const auto prefix = response_prefix_size(
          request_id, static_cast<uint16_t>(index), static_cast<uint16_t>(count));
      if (prefix >= kMaxWirePacketBytes) {
        valid = false;
        break;
      }
      offset += std::min(payload_size - offset, kMaxWirePacketBytes - prefix);
    }
    if (valid && offset >= payload_size) return count;
  }
  return kMaxChunks;
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

PasskeyDisclosureStatus LocalPasskeyHandoff::disclose_once(
    char* output, std::size_t capacity) noexcept {
  if (!configured_ || output == nullptr || capacity < 7U) {
    return configured_ && disclosed_ ? PasskeyDisclosureStatus::AlreadyDisclosed
                                     : PasskeyDisclosureStatus::Unavailable;
  }
  if (disclosed_) return PasskeyDisclosureStatus::AlreadyDisclosed;
  output[0] = static_cast<char>('0' + ((passkey_ / 100000U) % 10U));
  output[1] = static_cast<char>('0' + ((passkey_ / 10000U) % 10U));
  output[2] = static_cast<char>('0' + ((passkey_ / 1000U) % 10U));
  output[3] = static_cast<char>('0' + ((passkey_ / 100U) % 10U));
  output[4] = static_cast<char>('0' + ((passkey_ / 10U) % 10U));
  output[5] = static_cast<char>('0' + (passkey_ % 10U));
  output[6] = '\0';
  disclosed_ = true;
  return PasskeyDisclosureStatus::Ready;
}

BleParseStatus BleCommandParser::decode_frame(std::string_view frame,
                                              BleWireFrame& output) noexcept {
  output = {};
  while (!frame.empty() && (frame.back() == '\n' || frame.back() == '\r')) {
    frame.remove_suffix(1U);
  }
  if (frame.empty()) return BleParseStatus::Empty;
  if (frame.size() > kMaxFrameBytes) return BleParseStatus::Oversized;
  if (frame.substr(0U, 3U) != "F1|") {
    if (frame.size() > output.payload.size()) return BleParseStatus::Oversized;
    std::copy(frame.begin(), frame.end(), output.payload.begin());
    output.payload_size = frame.size();
    output.framed = false;
    return BleParseStatus::Accepted;
  }
  output.framed = true;
  if (frame.size() > kMaxWirePacketBytes) return BleParseStatus::Oversized;
  std::string_view field;
  std::size_t cursor = 3U;
  uint32_t request_id{0U};
  uint32_t index{0U};
  uint32_t count{0U};
  cursor = next_field(frame, cursor, field);
  if (cursor == std::string_view::npos ||
      !parse_uint(field, 0xFFFFFFFFU, request_id)) {
    return BleParseStatus::Invalid;
  }
  cursor = next_field(frame, cursor, field);
  if (cursor == std::string_view::npos ||
      !parse_uint(field, kMaxChunks - 1U, index)) {
    return BleParseStatus::Invalid;
  }
  cursor = next_field(frame, cursor, field);
  if (cursor == std::string_view::npos ||
      !parse_uint(field, kMaxChunks, count) || count == 0U || index >= count) {
    return BleParseStatus::Invalid;
  }
  const auto payload = frame.substr(cursor);
  if (payload.empty() || has_control(payload)) {
    return BleParseStatus::Invalid;
  }
  output.request_id = request_id;
  output.chunk_index = static_cast<uint16_t>(index);
  output.chunk_count = static_cast<uint16_t>(count);
  std::copy(payload.begin(), payload.end(), output.payload.begin());
  output.payload_size = payload.size();
  return BleParseStatus::Accepted;
}

BleParseStatus BleCommandParser::parse(std::string_view frame,
                                       BleCommand& output) noexcept {
  output = {};
  BleWireFrame wire{};
  const auto frame_status = decode_frame(frame, wire);
  if (frame_status != BleParseStatus::Accepted) return frame_status;
  frame = std::string_view(wire.payload.data(), wire.payload_size);
  while (!frame.empty() && (frame.front() == ' ' || frame.front() == '\t')) {
    frame.remove_prefix(1U);
  }
  if (frame.empty()) return BleParseStatus::Empty;
  if (frame.size() > kMaxFrameBytes) return BleParseStatus::Oversized;
  if (has_control(frame)) return BleParseStatus::Invalid;
  if (equal_literal(frame, "STATUS") || equal_literal(frame, "GET_STATUS")) {
    output.kind = BleCommandKind::Status;
    return BleParseStatus::Accepted;
  }
  if (equal_literal(frame, "SCAN") || equal_literal(frame, "SCAN_WIFI")) {
    output.kind = BleCommandKind::Scan;
    return BleParseStatus::Accepted;
  }
  if (equal_literal(frame, "PING")) {
    output.kind = BleCommandKind::Ping;
    return BleParseStatus::Accepted;
  }
  if (equal_literal(frame, "LIST_KNOWN_WIFI")) {
    output.kind = BleCommandKind::ListKnownWifi;
    return BleParseStatus::Accepted;
  }
  if (equal_literal(frame, "RETRY_WIFI")) {
    output.kind = BleCommandKind::RetryWifi;
    return BleParseStatus::Accepted;
  }
  constexpr std::string_view calibration_prefix{"CAL "};
  if (frame.substr(0U, calibration_prefix.size()) == calibration_prefix) {
    const auto command = frame.substr(calibration_prefix.size());
    const auto separator = command.find(' ');
    const auto operation = command.substr(0U, separator);
    if (operation.empty() || !calibration_operation(operation, output.kind)) {
      return BleParseStatus::Invalid;
    }
    if (command.size() >= output.calibration_arguments.size() ||
        has_control(command)) {
      return command.size() >= output.calibration_arguments.size()
                 ? BleParseStatus::Oversized
                 : BleParseStatus::Invalid;
    }
    std::copy(command.begin(), command.end(), output.calibration_arguments.begin());
    output.calibration_arguments_size = command.size();
    output.calibration_arguments[command.size()] = '\0';
    // Commands that need a target/criteria must carry one.  CAPABILITIES,
    // STATUS, HISTORY and CANCEL are valid without an argument because the
    // shared controller resolves the active session/target in that case.
    const bool requires_argument =
        output.kind == BleCommandKind::CalibrationEvidence ||
        output.kind == BleCommandKind::CalibrationActiveProfile ||
        output.kind == BleCommandKind::CalibrationStart ||
        output.kind == BleCommandKind::CalibrationCriteria ||
        output.kind == BleCommandKind::CalibrationCapture ||
        output.kind == BleCommandKind::CalibrationValidate ||
        output.kind == BleCommandKind::CalibrationPreview ||
        output.kind == BleCommandKind::CalibrationCommit ||
        output.kind == BleCommandKind::CalibrationRollback;
    if (requires_argument && separator == std::string_view::npos) {
      return BleParseStatus::Invalid;
    }
    return BleParseStatus::Accepted;
  }
  constexpr std::string_view prefix{"CONNECT "};
  constexpr std::string_view wifi_prefix{"CONNECT_WIFI "};
  if (frame.substr(0U, wifi_prefix.size()) == wifi_prefix) {
    frame.remove_prefix(wifi_prefix.size());
  } else if (frame.substr(0U, prefix.size()) == prefix) {
    frame.remove_prefix(prefix.size());
  } else {
    return BleParseStatus::Invalid;
  }
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
  scan_request_id_ = 0U;
  last_request_id_ = 0U;
  has_last_request_id_ = false;
  reset_reassembly();
  pending_response_size_ = 0U;
  pending_response_offset_ = 0U;
}

void BleProvisioningSession::on_disconnect() noexcept {
  connected_ = false;
  authenticated_ = false;
  scan_waiting_ = false;
  reset_reassembly();
  pending_response_size_ = 0U;
  pending_response_offset_ = 0U;
  if (calibration_backend_ != nullptr) calibration_backend_->disconnect();
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
  const auto size = std::min<std::size_t>(static_cast<std::size_t>(result),
                                          response.bytes.size() - 1U);
  queue_response(std::string_view(response.bytes.data(), size),
                 response_request_id_);
  emit_response(response);
}

void BleProvisioningSession::queue_response(std::string_view payload,
                                            uint32_t request_id) noexcept {
  pending_response_size_ = std::min<std::size_t>(payload.size(),
                                                 pending_response_.size());
  std::copy_n(payload.data(), pending_response_size_, pending_response_.data());
  pending_response_offset_ = 0U;
  pending_response_chunk_ = 0U;
  pending_response_id_ = request_id;
}

void BleProvisioningSession::emit_response(BleResponse& response) noexcept {
  response = {};
  if (pending_response_offset_ >= pending_response_size_) return;
  const auto remaining = pending_response_size_ - pending_response_offset_;
  const auto chunk_count = static_cast<uint16_t>(
      response_chunk_count(pending_response_size_, pending_response_id_));
  const int prefix = std::snprintf(
      response.bytes.data(), response.bytes.size(), "R1|%lu|%u|%u|",
      static_cast<unsigned long>(pending_response_id_),
      static_cast<unsigned>(pending_response_chunk_),
      static_cast<unsigned>(chunk_count));
  if (prefix <= 0 || static_cast<std::size_t>(prefix) >= response.bytes.size()) {
    pending_response_offset_ = pending_response_size_;
    return;
  }
  const auto available = std::min<std::size_t>(
      response.bytes.size() - static_cast<std::size_t>(prefix),
      kMaxWirePacketBytes - static_cast<std::size_t>(prefix));
  const auto copy_size = std::min(remaining, available);
  std::copy_n(pending_response_.data() + pending_response_offset_, copy_size,
              response.bytes.data() + prefix);
  response.size = static_cast<std::size_t>(prefix) + copy_size;
  if (response.size < response.bytes.size()) {
    response.bytes[response.size] = '\0';
  }
  response.notify = true;
  pending_response_offset_ += copy_size;
  ++pending_response_chunk_;
}

void BleProvisioningSession::reset_reassembly() noexcept {
  reassembly_size_ = 0U;
  reassembly_request_id_ = 0U;
  reassembly_next_chunk_ = 0U;
  reassembly_chunk_count_ = 0U;
  reassembly_active_ = false;
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
  if (pending_response_offset_ < pending_response_size_) {
    emit_response(response);
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
  // Reserve a fixed-width count so the header can be corrected after bounded
  // serialization without shifting the entry buffer.
  const int prefix = std::snprintf(response.bytes.data(), response.bytes.size(),
                                   "OK SCAN READY count=%02u", 0U);
  if (prefix <= 0) return;
  offset = static_cast<std::size_t>(prefix);
  std::size_t serialized_count{0U};
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
    ++serialized_count;
  }
  char header[32]{};
  const int final_prefix = std::snprintf(
      header, sizeof(header), "OK SCAN READY count=%02u",
      static_cast<unsigned>(serialized_count));
  if (final_prefix != prefix) return;
  std::memcpy(response.bytes.data(), header, static_cast<std::size_t>(prefix));
  response.size = offset;
  queue_response(std::string_view(response.bytes.data(), response.size),
                 scan_request_id_);
  emit_response(response);
}

BleParseStatus BleProvisioningSession::dispatch(std::string_view payload,
                                                uint32_t request_id,
                                                BleResponse& response) noexcept {
  response_request_id_ = request_id;
  BleCommand command{};
  const auto status = BleCommandParser::parse(payload, command);
  if (status != BleParseStatus::Accepted) {
    set_response(response, status == BleParseStatus::Oversized ? "ERR OVERSIZED"
                                                                 : "ERR INVALID");
    return status;
  }
  last_activity_us_ = clock_.now_us();
  if (calibration_command(command.kind)) {
    if (calibration_backend_ == nullptr) {
      set_response(response, "ERR CAL_UNAVAILABLE");
      return BleParseStatus::Accepted;
    }
    char command_text[136]{};
    const int command_size = std::snprintf(
        command_text, sizeof(command_text), "CAL %.*s",
        static_cast<int>(command.calibration_arguments_size),
        command.calibration_arguments.data());
    if (command_size <= 0 || static_cast<std::size_t>(command_size) >=
                                 sizeof(command_text)) {
      set_response(response, "ERR OVERSIZED");
      return BleParseStatus::Oversized;
    }
    std::size_t result_size{0U};
    const bool accepted = calibration_backend_->execute(
        std::string_view(command_text, static_cast<std::size_t>(command_size)),
        calibration_response_.data(), calibration_response_.size(), result_size);
    result_size = std::min(result_size, calibration_response_.size());
    if (result_size == 0U) {
      set_response(response, accepted ? "OK CAL" : "ERR CAL_REJECTED");
      return accepted ? BleParseStatus::Accepted : BleParseStatus::Invalid;
    }
    queue_response(std::string_view(calibration_response_.data(), result_size),
                   request_id);
    emit_response(response);
    return accepted ? BleParseStatus::Accepted : BleParseStatus::Invalid;
  }
  switch (command.kind) {
    case BleCommandKind::Status:
      status_response(response);
      break;
    case BleCommandKind::Ping:
      set_response(response, "OK PONG");
      break;
    case BleCommandKind::Scan:
      scan_request_id_ = request_id;
      scan_waiting_ = backend_.request_scan() == WifiScanStatus::Pending;
      scan_response(scan_waiting_ ? WifiScanStatus::Pending : WifiScanStatus::Failed,
                    response);
      break;
    case BleCommandKind::ListKnownWifi: {
      std::size_t count{0U};
      const auto scan_status = backend_.list_known_wifi(
          scan_results_.data(), scan_results_.size(), count);
      if (scan_status != WifiScanStatus::Ready) {
        set_response(response, "ERR WIFI_UNAVAILABLE");
        break;
      }
      std::size_t offset{0U};
      const int prefix = std::snprintf(response.bytes.data(), response.bytes.size(),
                                       "OK LIST_KNOWN_WIFI count=%u",
                                       static_cast<unsigned>(count));
      if (prefix <= 0) break;
      offset = static_cast<std::size_t>(prefix);
      for (std::size_t i = 0U; i < count && offset < response.bytes.size(); ++i) {
        const auto& item = scan_results_[i];
        const int written = std::snprintf(
            response.bytes.data() + offset, response.bytes.size() - offset,
            "\n%.*s,%u", 32, item.ssid.data(), item.secure ? 1U : 0U);
        if (written <= 0 || static_cast<std::size_t>(written) >=
                                response.bytes.size() - offset) break;
        offset += static_cast<std::size_t>(written);
      }
      queue_response(std::string_view(response.bytes.data(), offset), request_id);
      emit_response(response);
      break;
    }
    case BleCommandKind::RetryWifi:
      set_response(response, backend_.retry_wifi() ? "OK RETRY_WIFI"
                                                   : "ERR WIFI_UNAVAILABLE");
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

BleParseStatus BleProvisioningSession::receive(std::string_view frame,
                                               BleResponse& response) noexcept {
  response = {};
  BleWireFrame wire{};
  const auto frame_status = BleCommandParser::decode_frame(frame, wire);
  if (frame_status != BleParseStatus::Accepted) {
    set_response(response, frame_status == BleParseStatus::Oversized
                             ? "ERR OVERSIZED"
                             : "ERR INVALID");
    return frame_status;
  }
  response_request_id_ = wire.framed ? wire.request_id : 0U;
  if (!connected_) {
    set_response(response, "ERR DISCONNECTED");
    return BleParseStatus::Invalid;
  }
  if (!authenticated_) {
    set_response(response, "ERR AUTH_REQUIRED");
    return BleParseStatus::Invalid;
  }
  if (!wire.framed) {
    return dispatch(std::string_view(wire.payload.data(), wire.payload_size),
                    0U, response);
  }
  if (!reassembly_active_ && wire.chunk_index == 0U &&
      has_last_request_id_ && wire.request_id <= last_request_id_) {
    set_response(response, "ERR REPLAY");
    return BleParseStatus::Invalid;
  }
  if (!reassembly_active_) {
    if (wire.chunk_index != 0U) {
      set_response(response, "ERR FRAME_ORDER");
      return BleParseStatus::Invalid;
    }
    reassembly_active_ = true;
    reassembly_request_id_ = wire.request_id;
    reassembly_next_chunk_ = 0U;
    reassembly_chunk_count_ = wire.chunk_count;
    reassembly_size_ = 0U;
  }
  if (wire.request_id != reassembly_request_id_ ||
      wire.chunk_count != reassembly_chunk_count_ ||
      wire.chunk_index != reassembly_next_chunk_ ||
      reassembly_size_ + wire.payload_size > reassembly_.size()) {
    reset_reassembly();
    set_response(response, "ERR FRAME_ORDER");
    return BleParseStatus::Invalid;
  }
  std::copy_n(wire.payload.data(), wire.payload_size,
              reassembly_.data() + reassembly_size_);
  reassembly_size_ += wire.payload_size;
  ++reassembly_next_chunk_;
  if (reassembly_next_chunk_ != reassembly_chunk_count_) {
    set_response(response, "OK CHUNK");
    return BleParseStatus::Accepted;
  }
  const std::string_view payload(reassembly_.data(), reassembly_size_);
  last_request_id_ = reassembly_request_id_;
  has_last_request_id_ = true;
  const auto result = dispatch(payload, reassembly_request_id_, response);
  reset_reassembly();
  return result;
}

bool BleProvisioningSession::timed_out(uint64_t now_us) const noexcept {
  return now_us >= last_activity_us_ &&
         now_us - last_activity_us_ >=
             static_cast<uint64_t>(policy_.session_timeout_ms) * 1000U;
}

bool BleProvisioningSession::calibration_command(BleCommandKind kind) const noexcept {
  return static_cast<uint8_t>(kind) >=
         static_cast<uint8_t>(BleCommandKind::CalibrationCapabilities);
}

#ifdef ARDUINO
namespace {
class CommandCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit CommandCallbacks(ArduinoBleProvisioningService& owner) noexcept
      : owner_(owner) {}
  void onWrite(NimBLECharacteristic* characteristic,
               NimBLEConnInfo& conn_info) override;

 private:
  ArduinoBleProvisioningService& owner_;
};

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(ArduinoBleProvisioningService& owner) noexcept
      : owner_(owner) {}
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override;
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override;
  uint32_t onPassKeyDisplay() override;
  void onAuthenticationComplete(NimBLEConnInfo&) override;

 private:
  ArduinoBleProvisioningService& owner_;
};
}  // namespace

bool ArduinoBleProvisioningService::begin(BleProvisioningSession& session,
                                           std::string_view device_name) noexcept {
  if (device_name.empty() || device_name.size() > 31U) return false;
  char name[32]{};
  std::copy(device_name.begin(), device_name.end(), name);
  NimBLEDevice::init(std::string(name));
  std::array<uint8_t, kProvisioningSecretBytes> secret{};
  if (!secret_store_.load_or_create(secret)) return false;
  uint32_t passkey = 0U;
  for (std::size_t i = 0U; i < 4U; ++i) {
    passkey = (passkey << 8U) | static_cast<uint32_t>(secret[i]);
  }
  setup_passkey_ = passkey % 1000000U;
  if (setup_passkey_ == 0U) setup_passkey_ = 1U;
  passkey_handoff_.configure(setup_passkey_);
  // GATT reads/writes require an encrypted, authenticated link. NimBLE's
  // passkey and authentication callback provide the device-local proof of
  // possession; the setup secret never crosses the BLE wire.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_DISP_ONLY);
  NimBLEDevice::setSecurityPasskey(setup_passkey_);
  auto* server = NimBLEDevice::createServer();
  if (server == nullptr) return false;
  auto* service = server->createService(kProvisioningServiceUuid);
  if (service == nullptr) return false;
  auto* command = service->createCharacteristic(
      kProvisioningCommandUuid,
      NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_NR,
      192U);
  auto* response = service->createCharacteristic(
      kProvisioningResponseUuid,
      NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY,
      512U);
  if (command == nullptr || response == nullptr) return false;
  session_ = &session;
  server_ = server;
  command_ = command;
  response_ = response;
  command->setCallbacks(new CommandCallbacks(*this));
  server->setCallbacks(new ServerCallbacks(*this));
  server->getAdvertising()->addServiceUUID(kProvisioningServiceUuid);
  server->advertiseOnDisconnect(true);
  server->startAdvertising();
  return true;
}

void ArduinoBleProvisioningService::poll() noexcept {
  if (session_ == nullptr || response_ == nullptr) return;
  BleResponse response{};
  session_->poll(response);
  if (!response.notify || response.size == 0U) return;
  auto* characteristic = static_cast<NimBLECharacteristic*>(response_);
  characteristic->setValue(reinterpret_cast<uint8_t*>(response.bytes.data()),
                           response.size);
  characteristic->notify();
}

void CommandCallbacks::onWrite(NimBLECharacteristic* characteristic,
                               NimBLEConnInfo& conn_info) {
  owner_.handle_write(characteristic, conn_info.isEncrypted() &&
                                      conn_info.isAuthenticated());
}

void ServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo&) {
  owner_.handle_connect();
}

void ServerCallbacks::onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int) {
  owner_.handle_disconnect(server);
}

uint32_t ServerCallbacks::onPassKeyDisplay() { return owner_.setup_passkey(); }

void ServerCallbacks::onAuthenticationComplete(NimBLEConnInfo& conn_info) {
  owner_.handle_authenticated(conn_info.isEncrypted() &&
                              conn_info.isAuthenticated());
}

void ArduinoBleProvisioningService::handle_write(
    ::NimBLECharacteristic* characteristic, bool authenticated) noexcept {
  if (characteristic == nullptr || session_ == nullptr || response_ == nullptr) return;
  session_->on_authenticated(authenticated);
  const std::string value = static_cast<std::string>(characteristic->getValue());
  BleResponse result{};
  session_->receive(std::string_view(value.data(), value.size()), result);
  if (!result.notify || result.size == 0U) return;
  auto* response_characteristic = static_cast<NimBLECharacteristic*>(response_);
  response_characteristic->setValue(
      reinterpret_cast<uint8_t*>(result.bytes.data()), result.size);
  response_characteristic->notify();
}

void ArduinoBleProvisioningService::handle_connect() noexcept {
  if (session_ != nullptr) {
    session_->on_connect(static_cast<uint64_t>(esp_timer_get_time()));
  }
}

void ArduinoBleProvisioningService::handle_authenticated(
    bool authenticated) noexcept {
  if (session_ != nullptr) session_->on_authenticated(authenticated);
}

void ArduinoBleProvisioningService::handle_disconnect(::NimBLEServer* server) noexcept {
  if (session_ != nullptr) session_->on_disconnect();
  if (server != nullptr) server->startAdvertising();
}
#endif

}  // namespace sigap::provisioning
