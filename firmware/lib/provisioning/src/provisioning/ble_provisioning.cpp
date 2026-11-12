#include "ble_provisioning.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <charconv>

#ifdef ARDUINO
#include <NimBLECharacteristic.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <esp_timer.h>

static_assert(MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST) == 0,
              "BLE bond persistence must remain disabled");
static_assert(MYNEWT_VAL(BLE_STORE_MAX_BONDS) == 0,
              "BLE bond storage must remain excluded");
#endif

#if defined(ARDUINO) && defined(SIGAP_BLE_DIAGNOSTIC)
extern "C" bool sigap_ble_diag_failed() noexcept;
#endif
#if defined(ARDUINO) && defined(SIGAP_BLE_WIFI_TEST)
extern "C" bool sigap_ble_wifi_erase_attempted() noexcept;
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
  return 0U;
}

bool parse_u64(std::string_view value, uint64_t& output) noexcept {
  if (value.empty() || (value.size() > 1U && value.front() == '0')) return false;
  uint64_t parsed{};
  for (char byte : value) {
    if (byte < '0' || byte > '9') return false;
    const auto digit = static_cast<uint64_t>(byte - '0');
    if (parsed > (UINT64_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  output = parsed;
  return true;
}

bool parse_debug_boot(std::string_view value, acquisition::BootId& boot) noexcept {
  if (value.size() != 32U) return false;
  boot = {};
  boot.present = true;
  for (std::size_t i = 0; i < 16U; ++i) {
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    const int high = nibble(value[i * 2U]);
    const int low = nibble(value[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    boot.bytes[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
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
  if (equal_literal(frame, "GET_DEVICE_ID")) {
    output.kind = BleCommandKind::DeviceIdentity;
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
  if (frame == "GET_DEBUG META") { output.kind = BleCommandKind::DebugMeta; return BleParseStatus::Accepted; }
  constexpr std::string_view page_prefix{"GET_DEBUG PAGE "};
  if (frame.substr(0U,page_prefix.size())==page_prefix) {
    const auto args=frame.substr(page_prefix.size()); const auto split=args.find(' '); uint64_t after{};
    if(split==args.npos || !parse_debug_boot(args.substr(0U,split),output.debug_cursor.boot) ||
       !parse_u64(args.substr(split+1U),after)) return BleParseStatus::Invalid;
    output.debug_cursor.after=after; output.kind=BleCommandKind::DebugPage; return BleParseStatus::Accepted;
  }
  constexpr std::string_view level_prefix{"SET_DEBUG_LEVEL "};
  if(frame.substr(0U,level_prefix.size())==level_prefix) {
    const auto level=frame.substr(level_prefix.size());
    if(level=="ERROR")output.debug_level=debug::Level::Error;
    else if(level=="WARN")output.debug_level=debug::Level::Warn;
    else if(level=="INFO")output.debug_level=debug::Level::Info;
    else if(level=="DEBUG")output.debug_level=debug::Level::Debug;
    else return BleParseStatus::Invalid;
    output.kind=BleCommandKind::DebugLevel; return BleParseStatus::Accepted;
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
  const bool was_connected = connected_;
  disconnect_requested_ = false;
  connected_ = true;
  last_activity_us_ = now_us;
  scan_waiting_ = false;
  scan_request_id_ = 0U;
  last_request_id_ = 0U;
  has_last_request_id_ = false;
  reset_reassembly();
  pending_response_size_ = 0U;
  pending_response_offset_ = 0U;
  active_observation_ = pending_observation_ = scan_observation_ = {};
  if (!was_connected) emit_ble(10U, 0, 0);
}

void BleProvisioningSession::on_disconnect(BleCloseReason reason) noexcept {
  const bool was_connected = connected_;
  const int32_t pending = pending_request_count();
  connected_ = false;
  if (was_connected) emit_ble(11U, static_cast<int32_t>(reason), pending);
  scan_waiting_ = false;
  reset_reassembly();
  pending_response_size_ = 0U;
  pending_response_offset_ = 0U;
  if (calibration_backend_ != nullptr) calibration_backend_->disconnect();
  backend_.cancel();
}

void BleProvisioningSession::emit_ble(uint16_t code, int32_t arg0, int32_t arg1,
                                     const BleResponseObservation& context) noexcept {
  if (debug_sink_ == nullptr) return;
  debug::EventInput event{};
  event.module = debug::Module::Ble;
  event.event_code = code;
  event.level = (code == 12U || code == 13U) ? debug::Level::Debug :
      code == 15U ? (arg1 == 5 ? debug::Level::Error : debug::Level::Warn) : debug::Level::Info;
  event.monotonic_us = clock_.now_us();
  event.arg0 = arg0;
  event.arg1 = arg1;
  if (context.request_id_present) {
    event.presence = debug::kHasRequestId;
    event.request_id = context.request_id;
  }
  if (context.eligible && event.monotonic_us >= context.started_us) {
    event.duration_us = static_cast<uint32_t>(std::min<uint64_t>(
        event.monotonic_us - context.started_us, UINT32_MAX));
  }
  (void)debug_sink_->emit(event);
}

void BleProvisioningSession::observe_result(bool accepted, bool pending) noexcept {
  if (!active_observation_.eligible) return;
  if (!response_rendered_ || !response_geometry_ok_) {
    emit_ble(15U, 0, 5, active_observation_);
  } else if (!accepted) {
    emit_ble(15U, 0, 0, active_observation_);
  } else {
    emit_ble(14U, pending ? 1 : 0,
             static_cast<int32_t>(pending_response_size_), active_observation_);
  }
}

void BleProvisioningSession::observe_notification(const BleResponse& response,
                                                 bool admitted) noexcept {
  const auto& context = response.observation;
  if (!context.eligible || context.chunk_total == 0U ||
      context.chunk_index == last_notification_chunk_) return;
  last_notification_chunk_ = context.chunk_index;
  if (!admitted) {
    emit_ble(15U, 2, 0, context);
    return;
  }
  if (admitted_response_chunks_ < context.chunk_total) ++admitted_response_chunks_;
  emit_ble(13U, admitted_response_chunks_, context.chunk_total, context);
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
    response_rendered_ = false;
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
  // A valid uint32 ID does not make every logical response fit ATT geometry.
  if (payload.size() > pending_response_.size() ||
      response_chunk_count(payload.size(), request_id) == 0U) {
    payload = "ERR OVERSIZED";
    response_geometry_ok_ = false;
  }
  response_rendered_ = true;
  pending_observation_ = active_observation_;
  admitted_response_chunks_ = 0U;
  last_notification_chunk_ = UINT16_MAX;
  pending_response_size_ = payload.size();
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
  if (prefix <= 0 || static_cast<std::size_t>(prefix) >= kMaxWirePacketBytes ||
      static_cast<std::size_t>(prefix) >= response.bytes.size()) {
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
  response.observation = pending_observation_;
  response.observation.chunk_index = pending_response_chunk_;
  response.observation.chunk_total = chunk_count;
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
  if (read_only_diagnostic_) {
    set_response(response, "OK STATUS mode=BLE_DIAGNOSTIC wifi=DISABLED");
    return;
  }
  const auto ssid = backend_.current_ssid();
  set_response(response, "OK STATUS state=%s ssid=%.*s", state_name(backend_.wifi_state()),
               static_cast<int>(std::min<std::size_t>(ssid.size(), 32U)), ssid.data());
}

void BleProvisioningSession::device_identity_response(
    BleResponse& response) noexcept {
  if (device_id_ == 0U) {
    set_response(response, "ERR DEVICE_ID_UNAVAILABLE");
    return;
  }
  char identity[17]{};
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t index = 0U; index < 16U; ++index) {
    const auto shift = static_cast<unsigned>((15U - index) * 4U);
    identity[index] = kHex[(device_id_ >> shift) & 0x0fU];
  }
  set_response(response, "OK DEVICE_ID id=%s", identity);
}

void BleProvisioningSession::scan_response(WifiScanStatus status,
                                           BleResponse& response) noexcept {
  switch (status) {
    case WifiScanStatus::Pending:
      set_response(response, "OK SCAN PENDING");
      return;
    case WifiScanStatus::RateLimited:
      set_response(response, "ERR SCAN_RATE_LIMITED");
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
    emit_ble(16U, static_cast<int32_t>(std::min<uint64_t>(now - last_activity_us_, INT32_MAX)),
             pending_request_count());
    on_disconnect(BleCloseReason::Timeout);
    disconnect_requested_ = true;
    response = {};
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
  active_observation_ = scan_observation_;
  response_rendered_ = false;
  response_geometry_ok_ = true;
  if (status != WifiScanStatus::Ready) {
    response_request_id_ = scan_request_id_;
    scan_response(status, response);
    observe_result(false);
    return;
  }
  response = {};
  std::size_t offset{0U};
  // Reserve a fixed-width count so the header can be corrected after bounded
  // serialization without shifting the entry buffer.
  const int prefix = std::snprintf(response.bytes.data(), response.bytes.size(),
                                   "OK SCAN READY count=%02u", 0U);
  if (prefix <= 0) { observe_result(false); return; }
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
  if (final_prefix != prefix) { observe_result(false); return; }
  std::memcpy(response.bytes.data(), header, static_cast<std::size_t>(prefix));
  response.size = offset;
  queue_response(std::string_view(response.bytes.data(), response.size),
                 scan_request_id_);
  emit_response(response);
  observe_result(true);
}

BleParseStatus BleProvisioningSession::dispatch(std::string_view payload,
                                                uint32_t request_id,
                                                BleResponse& response, bool framed) noexcept {
  response_request_id_ = request_id;
  active_observation_ = {};
  active_observation_.request_id = request_id;
  active_observation_.request_id_present = framed;
  response_rendered_ = false;
  response_geometry_ok_ = true;
  BleCommand command{};
  const auto status = BleCommandParser::parse(payload, command);
  if (status != BleParseStatus::Accepted) {
    emit_ble(15U, 1, 3, active_observation_);
    set_response(response, status == BleParseStatus::Oversized ? "ERR OVERSIZED"
                                                                 : "ERR INVALID");
    return status;
  }
  last_activity_us_ = clock_.now_us();
  if (read_only_diagnostic_ && payload != "GET_STATUS" &&
      payload != "GET_DEVICE_ID" && payload != "PING") {
    set_response(response, "ERR DIAGNOSTIC_READ_ONLY");
    return BleParseStatus::Invalid;
  }
  active_observation_.eligible = command.kind == BleCommandKind::Scan ||
      command.kind == BleCommandKind::Connect || command.kind == BleCommandKind::RetryWifi ||
      command.kind == BleCommandKind::ListKnownWifi;
  active_observation_.started_us = last_activity_us_;
  if (active_observation_.eligible) {
    emit_ble(12U, 1, static_cast<int32_t>(payload.size()), active_observation_);
  }
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
  if (command.kind == BleCommandKind::DebugMeta || command.kind == BleCommandKind::DebugPage || command.kind == BleCommandKind::DebugLevel) {
    if (debug_backend_ == nullptr) { set_response(response,"ERR DEBUG_UNAVAILABLE"); return BleParseStatus::Accepted; }
    char output[384]{}; std::size_t length{}; bool ok{};
    if(command.kind==BleCommandKind::DebugMeta)ok=debug_backend_->meta(clock_.now_us(),output,sizeof(output),length);
    else if(command.kind==BleCommandKind::DebugPage)ok=debug_backend_->page(command.debug_cursor,output,sizeof(output),length);
    else ok=debug_backend_->set_level(command.debug_level,output,sizeof(output),length);
    if(!ok){set_response(response,"ERR DEBUG_UNAVAILABLE");return BleParseStatus::Accepted;}
    queue_response(std::string_view(output,length),request_id);emit_response(response);return BleParseStatus::Accepted;
  }
  bool accepted = true;
  bool pending = false;
  switch (command.kind) {
    case BleCommandKind::Status:
      status_response(response);
      break;
    case BleCommandKind::DeviceIdentity:
      device_identity_response(response);
      break;
    case BleCommandKind::Ping:
      set_response(response, "OK PONG");
      break;
    case BleCommandKind::Scan:
      scan_request_id_ = request_id;
      scan_observation_ = active_observation_;
      {
        const auto scan_status = backend_.request_scan();
        scan_waiting_ = scan_status == WifiScanStatus::Pending;
        pending = scan_waiting_;
        accepted = pending || scan_status == WifiScanStatus::Ready;
        scan_response(scan_status, response);
      }
      break;
    case BleCommandKind::ListKnownWifi: {
      std::size_t count{0U};
      const auto scan_status = backend_.list_known_wifi(
          scan_results_.data(), scan_results_.size(), count);
      if (scan_status != WifiScanStatus::Ready) {
        accepted = false;
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
      accepted = backend_.retry_wifi();
      set_response(response, accepted ? "OK RETRY_WIFI"
                                                   : "ERR WIFI_UNAVAILABLE");
      break;
    case BleCommandKind::Connect: {
      accepted = backend_.request_connect(
          std::string_view(command.ssid.data(), command.ssid_size),
          std::string_view(command.password.data(), command.password_size));
      set_response(response, accepted ? "OK CONNECTING" : "ERR CONNECT_REJECTED");
      break;
    }
  }
  observe_result(accepted, pending);
  return BleParseStatus::Accepted;
}

BleParseStatus BleProvisioningSession::receive(std::string_view frame,
                                               BleResponse& response) noexcept {
  response = {};
  active_observation_ = {};
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
  if (!wire.framed) {
    return dispatch(std::string_view(wire.payload.data(), wire.payload_size),
                    0U, response, false);
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
  const auto result = dispatch(payload, reassembly_request_id_, response, true);
  reset_reassembly();
  return result;
}

bool BleProvisioningSession::timed_out(uint64_t now_us) const noexcept {
  return now_us >= last_activity_us_ &&
         now_us - last_activity_us_ >=
             static_cast<uint64_t>(policy_.session_timeout_ms) * 1000U;
}

bool BleProvisioningSession::calibration_command(BleCommandKind kind) const noexcept {
  switch (kind) {
    case BleCommandKind::CalibrationCapabilities:
    case BleCommandKind::CalibrationStatus:
    case BleCommandKind::CalibrationEvidence:
    case BleCommandKind::CalibrationActiveProfile:
    case BleCommandKind::CalibrationStart:
    case BleCommandKind::CalibrationCriteria:
    case BleCommandKind::CalibrationCapture:
    case BleCommandKind::CalibrationValidate:
    case BleCommandKind::CalibrationPreview:
    case BleCommandKind::CalibrationCommit:
    case BleCommandKind::CalibrationCancel:
    case BleCommandKind::CalibrationHistory:
    case BleCommandKind::CalibrationRollback: return true;
    default: return false;
  }
}

#ifdef ARDUINO
namespace {
class QueueLock final {
 public:
  explicit QueueLock(portMUX_TYPE& mux) noexcept : mux_(mux) {
    portENTER_CRITICAL(&mux_);
  }
  ~QueueLock() { portEXIT_CRITICAL(&mux_); }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;
 private:
  portMUX_TYPE& mux_;
};

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

 private:
  ArduinoBleProvisioningService& owner_;
};

#ifdef SIGAP_BLE_DIAGNOSTIC
const char* diagnostic_storage_failure_reason() noexcept {
  return "nvs_error";
}
#endif
}  // namespace

bool ArduinoBleProvisioningService::begin(BleProvisioningSession& session,
                                           std::string_view device_name) noexcept {
  if (device_name.empty() || device_name.size() > 31U) {
#ifdef SIGAP_BLE_DIAGNOSTIC
    start_failure_reason_ = "invalid_device_name";
#endif
    return false;
  }
  char name[32]{};
  std::copy(device_name.begin(), device_name.end(), name);
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (!NimBLEDevice::init(std::string(name)) || sigap_ble_diag_failed()) {
    start_failure_reason_ = sigap_ble_diag_failed()
                                 ? diagnostic_storage_failure_reason()
                                 : "nimble_init_failed";
    return false;
  }
#else
  NimBLEDevice::init(std::string(name));
#endif
  // This prototype flow has no BLE authentication or bonding. GATT access
  // does not request encryption; device ID is routing data, not proof.
  NimBLEDevice::setSecurityAuth(false, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  auto* server = NimBLEDevice::createServer();
  if (server == nullptr) {
#ifdef SIGAP_BLE_DIAGNOSTIC
    start_failure_reason_ = "server_init_failed";
#endif
    return false;
  }
  auto* service = server->createService(kProvisioningServiceUuid);
  if (service == nullptr) {
#ifdef SIGAP_BLE_DIAGNOSTIC
    start_failure_reason_ = "service_init_failed";
#endif
    return false;
  }
  auto* command = service->createCharacteristic(
      kProvisioningCommandUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
      192U);
  auto* response = service->createCharacteristic(
      kProvisioningResponseUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
      512U);
  if (command == nullptr || response == nullptr) {
#ifdef SIGAP_BLE_DIAGNOSTIC
    start_failure_reason_ = "characteristic_init_failed";
#endif
    return false;
  }
  session_ = &session;
  server_ = server;
  command_ = command;
  response_ = response;
  command->setCallbacks(new CommandCallbacks(*this));
  server->setCallbacks(new ServerCallbacks(*this));
  auto* advertising = server->getAdvertising();
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (advertising == nullptr) {
    start_failure_reason_ = "advertising_init_failed";
    return false;
  }
#endif
  advertising->addServiceUUID(kProvisioningServiceUuid);
  server->advertiseOnDisconnect(true);
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (sigap_ble_diag_failed()) {
    start_failure_reason_ = diagnostic_storage_failure_reason();
    return false;
  }
#endif
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (!server->startAdvertising()) {
    start_failure_reason_ = "advertising_start_failed";
    return false;
  }
  if (sigap_ble_diag_failed()) {
    (void)server->stopAdvertising();
    server->advertiseOnDisconnect(false);
    start_failure_reason_ = diagnostic_storage_failure_reason();
    return false;
  }
  fail_closed_ = false;
  start_failure_reason_ = "none";
#else
  server->startAdvertising();
#endif
  return true;
}

void ArduinoBleProvisioningService::poll() noexcept {
  if (session_ == nullptr || response_ == nullptr) return;
#ifdef SIGAP_BLE_WIFI_TEST
  if (sigap_ble_wifi_erase_attempted()) {
    if (!wifi_test_fail_closed_) {
      wifi_test_fail_closed_ = true;
      close_session();
      auto* server = static_cast<NimBLEServer*>(server_);
      if (server != nullptr) {
        server->advertiseOnDisconnect(false);
        (void)server->stopAdvertising();
        disconnect_connection(callback_connection());
      }
    }
    return;
  }
#endif
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (sigap_ble_diag_failed()) {
    if (!fail_closed_) {
      fail_closed_ = true;
      close_session();
      auto* server = static_cast<NimBLEServer*>(server_);
      if (server != nullptr) {
        server->advertiseOnDisconnect(false);
        (void)server->stopAdvertising();
        disconnect_connection(callback_connection());
      }
    }
    return;
  }
#endif
  BleServiceConnection expected;
  {
    QueueLock lock(queue_mux_);
    expected = queue_.connection();
    disconnect_observed_ = false;
  }
  auto* presence_server = static_cast<NimBLEServer*>(server_);
  const uint16_t mtu = expected.connected && !expected.failed && presence_server != nullptr
      ? presence_server->getPeerMTU(expected.handle) : 0U;
  bool query_interrupted;
  {
    QueueLock lock(queue_mux_);
    query_interrupted = disconnect_observed_;
    disconnect_observed_ = false;
  }
  if (query_interrupted) return;
  if (expected.connected && !expected.failed && presence_server != nullptr && mtu == 0U) {
    QueueLock lock(queue_mux_);
    const auto current = queue_.connection();
    if (current.connected && !current.failed &&
        current.epoch == expected.epoch && current.handle == expected.handle) {
      queue_.disconnect(expected.handle, static_cast<uint64_t>(esp_timer_get_time()));
    }
  }
  BleServiceConnection connection;
  bool failed;
  {
    QueueLock lock(queue_mux_);
    connection = queue_.connection();
    failed = queue_.consume_failure();
  }
  if (session_owned_ && (!connection.connected || connection.failed ||
      connection.epoch != active_connection_.epoch ||
      connection.handle != active_connection_.handle)) close_session();
  if (failed) {
    disconnect_connection(connection);
    return;
  }
  for (size_t count = 0; count < 4; ++count) {
    BleServiceEvent event;
    bool valid;
    {
      QueueLock lock(queue_mux_);
      if (disconnect_observed_) return;
      if (!queue_.pop(event)) break;
      valid = queue_.matches(event);
    }
    if (!valid) continue;
    if (event.kind == BleServiceEventKind::Disconnect) {
      close_session();
    } else if (event.kind == BleServiceEventKind::Connect) {
      close_session();
      active_connection_ = {event.epoch, event.handle, true, false};
      session_->on_connect(event.received_us);
      session_owned_ = true;
    } else if (session_owned_ && event.epoch == active_connection_.epoch &&
               event.handle == active_connection_.handle &&
               current_connection(active_connection_)) {
      BleResponse response{};
      session_->receive(std::string_view(event.payload.data(), event.size), response);
      if (session_->consume_disconnect_request()) {
        close_session();
        disconnect_connection(active_connection_);
        return;
      }
      notify(response, active_connection_);
    }
  }
  if (!session_owned_ || !current_connection(active_connection_)) return;
  BleResponse response{};
  session_->poll(response);
  if (session_->consume_disconnect_request()) {
    close_session();
    disconnect_connection(active_connection_);
    return;
  }
  notify(response, active_connection_);
}

void CommandCallbacks::onWrite(NimBLECharacteristic* characteristic,
                               NimBLEConnInfo& conn_info) {
  owner_.handle_write(characteristic, conn_info.getConnHandle());
}

void ServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo& conn_info) {
  owner_.handle_connect(conn_info.getConnHandle());
}

void ServerCallbacks::onDisconnect(NimBLEServer* server, NimBLEConnInfo& conn_info, int) {
  owner_.handle_disconnect(server, conn_info.getConnHandle());
}

void ArduinoBleProvisioningService::handle_write(
    ::NimBLECharacteristic* characteristic) noexcept {
  handle_write(characteristic, callback_connection().handle);
}

void ArduinoBleProvisioningService::handle_write(
    ::NimBLECharacteristic* characteristic, uint16_t connection_handle) noexcept {
  if (characteristic == nullptr || session_ == nullptr || response_ == nullptr) return;
#ifdef SIGAP_BLE_WIFI_TEST
  if (sigap_ble_wifi_erase_attempted()) return;
#endif
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (sigap_ble_diag_failed()) return;
#endif
  auto value = characteristic->getValue();
  {
    QueueLock lock(queue_mux_);
    queue_.write(connection_handle,
        std::string_view(reinterpret_cast<const char*>(value.data()), value.length()),
        static_cast<uint64_t>(esp_timer_get_time()));
  }
  // getValue() owns a deep copy; scrub it before the SDK releases that buffer.
  volatile uint8_t* bytes = const_cast<uint8_t*>(value.data());
  for (size_t i = 0; i < value.length(); ++i) bytes[i] = 0;
}

void ArduinoBleProvisioningService::handle_connect(
    uint16_t connection_handle) noexcept {
#ifdef SIGAP_BLE_WIFI_TEST
  if (sigap_ble_wifi_erase_attempted()) return;
#endif
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (sigap_ble_diag_failed()) return;
#endif
  if (session_ == nullptr) return;
  QueueLock lock(queue_mux_);
  queue_.connect(connection_handle, static_cast<uint64_t>(esp_timer_get_time()));
}


void ArduinoBleProvisioningService::handle_disconnect(::NimBLEServer* server) noexcept {
  handle_disconnect(server, callback_connection().handle);
}

void ArduinoBleProvisioningService::handle_disconnect(
    ::NimBLEServer* server, uint16_t connection_handle) noexcept {
  (void)server;
#ifdef SIGAP_BLE_WIFI_TEST
  if (sigap_ble_wifi_erase_attempted()) return;
#endif
#ifdef SIGAP_BLE_DIAGNOSTIC
  if (sigap_ble_diag_failed()) return;
#endif
  (void)connection_handle;
  // Disconnect callbacks do not identify an originating connection generation.
  // They only invalidate an in-flight point-in-time presence observation.
  (void)server;
  if (session_ == nullptr) return;
  QueueLock lock(queue_mux_);
  disconnect_observed_ = true;
}

BleServiceConnection ArduinoBleProvisioningService::callback_connection() noexcept {
  QueueLock lock(queue_mux_);
  return queue_.connection();
}

bool ArduinoBleProvisioningService::current_connection(
    const BleServiceConnection& expected) noexcept {
  const auto current = callback_connection();
  return current.connected && !current.failed && current.epoch == expected.epoch &&
         current.handle == expected.handle;
}

void ArduinoBleProvisioningService::close_session() noexcept {
  if (session_owned_ && session_->connected()) session_->on_disconnect();
  session_owned_ = false;
}

void ArduinoBleProvisioningService::disconnect_connection(
    const BleServiceConnection& expected) noexcept {
  bool terminate;
  {
    QueueLock lock(queue_mux_);
    terminate = queue_.terminate(expected);
  }
  auto* server = static_cast<NimBLEServer*>(server_);
  if (terminate && server != nullptr && expected.handle != 0xFFFFU)
    (void)server->disconnect(expected.handle);
}

void ArduinoBleProvisioningService::notify(
    const BleResponse& response, const BleServiceConnection& expected) noexcept {
  if (!response.notify || response.size == 0 || !current_connection(expected)) return;
  auto* characteristic = static_cast<NimBLECharacteristic*>(response_);
  characteristic->setValue(reinterpret_cast<const uint8_t*>(response.bytes.data()), response.size);
  const bool admitted = characteristic->notify(expected.handle);
  session_->observe_notification(response, admitted);
}
#endif

}  // namespace sigap::provisioning
