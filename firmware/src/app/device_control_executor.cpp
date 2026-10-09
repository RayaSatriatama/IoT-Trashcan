#include "device_control_executor.h"

#ifdef ARDUINO

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

#include "network/device_control_codec.h"

namespace sigap {
namespace {

bool append_text(network::DeviceControlResult& output,
                 std::string_view value) noexcept {
  if (output.payload_size + value.size() >= output.payload.size()) return false;
  std::copy(value.begin(), value.end(),
            output.payload.begin() + output.payload_size);
  output.payload_size += value.size();
  return true;
}

bool append_hex(network::DeviceControlResult& output,
                const std::array<uint8_t, 32U>& value) noexcept {
  static constexpr char digits[] = "0123456789abcdef";
  for (const uint8_t byte : value) {
    const char pair[2] = {digits[byte >> 4U], digits[byte & 0x0fU]};
    if (!append_text(output, std::string_view(pair, 2U))) return false;
  }
  return true;
}

// Locate exactly one key in the root JSON object. Substring lookup is not
// sufficient for device control because a nested or duplicate field could
// otherwise change the meaning of a command while retaining the same digest.
bool root_key(std::string_view payload, std::string_view key,
              std::size_t& value_start) noexcept {
  std::size_t begin = 0U;
  while (begin < payload.size() &&
         (payload[begin] == ' ' || payload[begin] == '\t' ||
          payload[begin] == '\r' || payload[begin] == '\n')) ++begin;
  if (begin >= payload.size() || payload[begin] != '{') return false;
  int depth{0};
  bool in_string{false};
  bool escaped{false};
  bool found{false};
  for (std::size_t index = begin; index < payload.size(); ++index) {
    const char byte = payload[index];
    if (in_string) {
      if (escaped) escaped = false;
      else if (byte == '\\') escaped = true;
      else if (byte == '"') in_string = false;
      continue;
    }
    if (byte == '"') {
      const std::size_t token_begin = index + 1U;
      std::size_t token_end = token_begin;
      bool token_escaped = false;
      for (; token_end < payload.size(); ++token_end) {
        if (token_escaped) token_escaped = false;
        else if (payload[token_end] == '\\') token_escaped = true;
        else if (payload[token_end] == '"') break;
      }
      if (token_end >= payload.size()) return false;
      std::size_t cursor = token_end + 1U;
      while (cursor < payload.size() &&
             (payload[cursor] == ' ' || payload[cursor] == '\t' ||
              payload[cursor] == '\r' || payload[cursor] == '\n')) ++cursor;
      if (depth == 1 && cursor < payload.size() && payload[cursor] == ':') {
        if (payload.substr(token_begin, token_end - token_begin) == key) {
          if (found) return false;
          found = true;
          value_start = cursor + 1U;
        }
      }
      index = token_end;
      continue;
    }
    if (byte == '{' || byte == '[') ++depth;
    else if (byte == '}' || byte == ']') {
      if (depth == 0) return false;
      --depth;
    }
  }
  return found && depth == 0;
}

}  // namespace

uint64_t ApplicationCalibrationExecutor::stable_id(std::string_view value) noexcept {
  uint64_t hash{1469598103934665603ULL};
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash == 0U ? 1U : hash;
}

bool ApplicationCalibrationExecutor::sensor_target(
    std::string_view value, calibration::CalibrationTarget& output) noexcept {
  constexpr std::array<std::pair<std::string_view, calibration::CalibrationTarget>, 13U>
      entries{{
          {"hx711_organic", calibration::CalibrationTarget::Hx711Organic},
          {"hx711_inorganic", calibration::CalibrationTarget::Hx711Inorganic},
          {"hx711_b3", calibration::CalibrationTarget::Hx711B3},
          {"ultrasonic_organic", calibration::CalibrationTarget::UltrasonicOrganic},
          {"ultrasonic_inorganic", calibration::CalibrationTarget::UltrasonicInorganic},
          {"ultrasonic_b3", calibration::CalibrationTarget::UltrasonicB3},
          {"mq4", calibration::CalibrationTarget::Mq4},
          {"mq135", calibration::CalibrationTarget::Mq135},
          {"mq7", calibration::CalibrationTarget::Mq7},
          {"mq8", calibration::CalibrationTarget::Mq8},
          {"ds18b20_b3", calibration::CalibrationTarget::Ds18b20B3},
          {"sht31_44", calibration::CalibrationTarget::Sht31_44},
          {"sht31_45", calibration::CalibrationTarget::Sht31_45},
      }};
  for (const auto& entry : entries) {
    if (entry.first == value) {
      output = entry.second;
      return true;
    }
  }
  return false;
}

bool ApplicationCalibrationExecutor::json_number(
    std::string_view payload, std::string_view key, int64_t& output) noexcept {
  std::size_t cursor{0U};
  if (!root_key(payload, key, cursor)) return false;
  while (cursor < payload.size() && (payload[cursor] == ' ' || payload[cursor] == '\t')) ++cursor;
  const auto begin = cursor;
  if (cursor < payload.size() && payload[cursor] == '-') ++cursor;
  while (cursor < payload.size() && payload[cursor] >= '0' && payload[cursor] <= '9') ++cursor;
  if (cursor == begin || (cursor == begin + 1U && payload[begin] == '-')) return false;
  const auto result = std::from_chars(payload.data() + begin, payload.data() + cursor, output);
  if (result.ec != std::errc{} || result.ptr != payload.data() + cursor) return false;
  while (cursor < payload.size() && (payload[cursor] == ' ' || payload[cursor] == '\t' ||
                                     payload[cursor] == '\r' || payload[cursor] == '\n')) ++cursor;
  return cursor < payload.size() && (payload[cursor] == ',' || payload[cursor] == '}');
}

bool ApplicationCalibrationExecutor::json_string(
    std::string_view payload, std::string_view key, std::string_view& output) noexcept {
  std::size_t cursor{0U};
  if (!root_key(payload, key, cursor)) return false;
  while (cursor < payload.size() && (payload[cursor] == ' ' || payload[cursor] == '\t')) ++cursor;
  if (cursor >= payload.size() || payload[cursor] != '"') return false;
  const auto begin = ++cursor;
  const auto end = payload.find('"', begin);
  if (end == std::string_view::npos || end == begin) return false;
  output = payload.substr(begin, end - begin);
  if (output.find_first_of("\\\r\n") != std::string_view::npos) return false;
  std::size_t cursor_after = end + 1U;
  while (cursor_after < payload.size() &&
         (payload[cursor_after] == ' ' || payload[cursor_after] == '\t' ||
          payload[cursor_after] == '\r' || payload[cursor_after] == '\n')) ++cursor_after;
  return cursor_after < payload.size() &&
         (payload[cursor_after] == ',' || payload[cursor_after] == '}');
}

bool parse_sha256(std::string_view value,
                  std::array<uint8_t, 32U>& output) noexcept {
  constexpr std::string_view prefix{"sha256:"};
  if (value.substr(0U, prefix.size()) != prefix ||
      value.size() != prefix.size() + output.size() * 2U) return false;
  auto nibble = [](char byte) noexcept -> int {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0U; index < output.size(); ++index) {
    const int high = nibble(value[prefix.size() + index * 2U]);
    const int low = nibble(value[prefix.size() + index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool ApplicationCalibrationExecutor::digest_payload(
    const network::DeviceControlCommand& command,
    calibration::CalibrationCommandEnvelope& envelope) noexcept {
  if (command.payload_size > UINT16_MAX || command.request_hash.size == 0U ||
      !parse_sha256(command.request_hash.view(), envelope.payload_digest)) {
    return false;
  }
  envelope.payload_size = static_cast<uint16_t>(command.payload_size);
  return true;
}

bool ApplicationCalibrationExecutor::dispatch_line(
    std::string_view line, uint64_t now_us,
    maintenance::MaintenanceResponse& response) noexcept {
  parser_.reset();
  maintenance::ParseEvent event{};
  for (const char byte : line) {
    if (parser_.consume(byte, event)) return false;
  }
  if (!parser_.consume('\n', event) ||
      event.status != maintenance::CommandStatus::Accepted) return false;
  return router_.execute(event.command, now_us, response) ==
         maintenance::CommandStatus::Accepted;
}

bool ApplicationCalibrationExecutor::apply_setup_fields(
    const network::DeviceControlCommand& command,
    calibration::CalibrationTarget target, uint64_t now_us) noexcept {
  const std::string_view payload(
      reinterpret_cast<const char*>(command.payload.data()), command.payload_size);
  constexpr std::array<std::string_view, 12U> keys{{
      "reference_mass_mg", "reference_temperature_centi_c",
      "reference_humidity_centi_pct", "resolution_bits", "echo_mm_per_us_milli",
      "echo_timeout_us", "inter_sensor_gap_us", "overshoot_tolerance_mm_milli",
      "reference_mv", "full_scale_counts", "divider_numerator",
      "divider_denominator"}};
  (void)target;
  for (const auto key : keys) {
    int64_t value{0};
    if (!json_number(payload, key, value)) continue;
    std::array<char, 128U> line{};
    const int written = std::snprintf(line.data(), line.size(),
                                      "calibration criteria %.*s %lld",
                                      static_cast<int>(key.size()), key.data(),
                                      static_cast<long long>(value));
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) return false;
    maintenance::MaintenanceResponse response{};
    if (!dispatch_line(std::string_view(line.data(), static_cast<std::size_t>(written)),
                       now_us, response)) return false;
  }
  return true;
}

bool ApplicationCalibrationExecutor::append_result_json(
    const calibration::CalibrationResultEnvelope& source, std::string_view sensor,
    network::DeviceControlResult& output) noexcept {
  output.payload_size = 0U;
  if ((source.status == calibration::CalibrationCommandStatus::Committed ||
       source.status == calibration::CalibrationCommandStatus::Replayed) &&
      source.profile.present && source.profile.version != 0U) {
    char version[24]{};
    const auto converted = std::to_chars(version, version + sizeof(version), source.profile.version);
    if (converted.ec != std::errc{} ||
        !append_text(output, "{\"profile_key\":\"") ||
        !append_text(output, sensor) || !append_text(output, "\",\"profile_hash\":\"sha256:") ||
        !append_hex(output, source.profile.hash) || !append_text(output, "\",\"version\":") ||
        !append_text(output, std::string_view(version, converted.ptr - version)) ||
        !append_text(output, "}")) return false;
    return true;
  }
  return append_text(output, "{\"status\":\"ACCEPTED\"}");
}

bool ApplicationCalibrationExecutor::append_preview_json(
    std::string_view sensor, const maintenance::MaintenanceResponse& preview,
    network::DeviceControlResult& output) noexcept {
  output.payload_size = 0U;
  if (!append_text(output, "{\"status\":\"PREVIEW_READY\",\"sensor_id\":\"")) return false;
  if (!append_text(output, sensor) || !append_text(output, "\",\"proposal_hash\":\"")) return false;
  std::string_view lines = preview.view();
  std::string_view proposal_hash{};
  std::string_view profile_version{};
  std::string_view evidence_hash{};
  std::string_view evidence_count{};
  std::string_view expires_in_ms{};
  std::array<std::pair<std::string_view, std::string_view>, 16U> parameters{};
  std::size_t parameter_count{0U};
  while (!lines.empty()) {
    const auto end = lines.find('\n');
    const auto line = lines.substr(0U, end == std::string_view::npos ? lines.size() : end);
    if (end == std::string_view::npos) lines = {};
    else lines.remove_prefix(end + 1U);
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) return false;
    const auto key = line.substr(0U, equals);
    const auto value = line.substr(equals + 1U);
    if (key == "proposal_hash") proposal_hash = value;
    else if (key == "profile_version") profile_version = value;
    else if (key == "evidence_hash") evidence_hash = value;
    else if (key == "evidence_count") evidence_count = value;
    else if (key == "expires_in_ms") expires_in_ms = value;
    else if (parameter_count < parameters.size()) parameters[parameter_count++] = {key, value};
    else return false;
  }
  if (proposal_hash.empty() || profile_version.empty() || evidence_hash.empty() ||
      evidence_count.empty() || expires_in_ms.empty() ||
      !append_text(output, proposal_hash) || !append_text(output, "\",\"profile_version\":\"") ||
      !append_text(output, profile_version) || !append_text(output, "\",\"evidence_count\":") ||
      !append_text(output, evidence_count) || !append_text(output, ",\"evidence_hash\":\"") ||
      !append_text(output, evidence_hash) || !append_text(output, "\",\"expires_in_ms\":") ||
      !append_text(output, expires_in_ms) || !append_text(output, ",\"parameters\":{")) return false;
  for (std::size_t index = 0U; index < parameter_count; ++index) {
    if (index != 0U && !append_text(output, ",")) return false;
    if (!append_text(output, "\"") || !append_text(output, parameters[index].first) ||
        !append_text(output, "\":") || !append_text(output, parameters[index].second)) return false;
  }
  return append_text(output, "}}");
}

bool ApplicationCalibrationExecutor::execute(
    const network::DeviceControlCommand& command, uint64_t now_us,
    network::DeviceControlResult& result) noexcept {
  result = network::DeviceControlResult{};
  (void)result.command_id.assign(command.command_id.view());
  (void)result.session_id.assign(command.session_id.view());
  calibration::CalibrationTarget target{};
  if (!sensor_target(command.sensor_id.view(), target)) return false;
  calibration::CalibrationCommandEnvelope envelope{};
  envelope.request_id = stable_id(command.command_id.view());
  uint64_t device_id{0U};
  const auto device_parse = std::from_chars(
      command.device_id.view().data(),
      command.device_id.view().data() + command.device_id.view().size(),
      device_id, 16);
  if (device_parse.ec != std::errc{} ||
      device_parse.ptr != command.device_id.view().data() + command.device_id.view().size() ||
      device_id == 0U) return false;
  envelope.device_id = device_id;
  envelope.principal_id = stable_id(command.operator_id.view());
  envelope.session_id = stable_id(command.session_id.view());
  envelope.expires_at_us = command.expires_at_us;
  envelope.transport = calibration::CalibrationTransport::Https;
  envelope.target = target;
  if (!digest_payload(command, envelope)) {
    return false;
  }
  const auto operation = command.operation.view();
  if (operation == "start") envelope.operation = calibration::CalibrationOperation::Start;
  else if (operation == "set_criteria" || operation == "criteria") envelope.operation = calibration::CalibrationOperation::SetCriteria;
  else if (operation == "capture") envelope.operation = calibration::CalibrationOperation::Capture;
  else if (operation == "validate") envelope.operation = calibration::CalibrationOperation::Validate;
  else if (operation == "preview") envelope.operation = calibration::CalibrationOperation::Preview;
  else if (operation == "commit") envelope.operation = calibration::CalibrationOperation::Commit;
  else if (operation == "cancel") envelope.operation = calibration::CalibrationOperation::Cancel;
  else return false;

  const std::string_view payload(reinterpret_cast<const char*>(command.payload.data()), command.payload_size);
  std::string_view expected_hash{};
  const bool has_expected_hash =
      payload.find("\"expected_profile_hash\"") != std::string_view::npos;
  const bool has_expected_version =
      payload.find("\"expected_profile_version\"") != std::string_view::npos;
  if (has_expected_hash != has_expected_version ||
      (envelope.operation == calibration::CalibrationOperation::Commit &&
       !has_expected_hash)) {
    return false;
  }
  if (has_expected_hash) {
    uint64_t expected_version{0U};
    if (!json_string(payload, "expected_profile_hash", expected_hash) ||
        !network::parse_json_canonical_uint64_field(
            payload, "expected_profile_version", expected_version) ||
        !parse_sha256(expected_hash, envelope.expected_profile.hash)) return false;
    envelope.expected_profile.present = true;
    envelope.expected_profile.version = expected_version;
    envelope.expected_profile_present = true;
  }
  if (envelope.operation == calibration::CalibrationOperation::SetCriteria) {
    calibration::CaptureCriteria criteria{calibration::Hx711CaptureCriteria{}};
    int64_t samples{0};
    int64_t span{0};
    if (!json_number(payload, "samples", samples) || samples <= 0 || samples > UINT16_MAX) return false;
    if (target >= calibration::CalibrationTarget::UltrasonicOrganic && target <= calibration::CalibrationTarget::UltrasonicB3) {
      if (!json_number(payload, "max_span_mm", span)) return false;
      criteria = calibration::DistanceCaptureCriteria{static_cast<uint16_t>(samples), static_cast<float>(span)};
    } else if (target >= calibration::CalibrationTarget::Mq4 && target <= calibration::CalibrationTarget::Mq8) {
      if (!json_number(payload, "max_span_counts", span) || span < 0 || span > UINT16_MAX) return false;
      criteria = calibration::MqCaptureCriteria{static_cast<uint16_t>(samples), static_cast<uint16_t>(span), target == calibration::CalibrationTarget::Mq7};
    } else if (target == calibration::CalibrationTarget::Ds18b20B3) {
      if (!json_number(payload, "max_span_centi_c", span)) return false;
      criteria = calibration::Ds18b20CaptureCriteria{static_cast<uint16_t>(samples), static_cast<int32_t>(span), true};
    } else if (target == calibration::CalibrationTarget::Sht31_44 || target == calibration::CalibrationTarget::Sht31_45) {
      int64_t humidity{0};
      if (!json_number(payload, "max_temperature_span_centi_c", span) || !json_number(payload, "max_humidity_span_centi_pct", humidity)) return false;
      criteria = calibration::Sht31CaptureCriteria{static_cast<uint16_t>(samples), static_cast<int32_t>(span), static_cast<int32_t>(humidity)};
    } else {
      if (!json_number(payload, "max_span_counts", span)) return false;
      criteria = calibration::Hx711CaptureCriteria{static_cast<uint16_t>(samples), span};
    }
    envelope.criteria = criteria;
    envelope.criteria_present = true;
  }
  if (envelope.operation == calibration::CalibrationOperation::Capture) {
    std::string_view anchor{};
    if (!json_string(payload, "anchor", anchor)) return false;
    envelope.anchor = anchor == "tare" ? calibration::CalibrationAnchor::Tare :
                      anchor == "reference" ? calibration::CalibrationAnchor::Reference :
                      anchor == "empty" ? calibration::CalibrationAnchor::Empty :
                      anchor == "full" ? calibration::CalibrationAnchor::Full : calibration::CalibrationAnchor::Primary;
  }
  calibration::CalibrationResultEnvelope response{};
  if (!controller_.execute(envelope, now_us, response)) return false;
  if (response.status == calibration::CalibrationCommandStatus::Replayed &&
      response.profile.present &&
      envelope.operation == calibration::CalibrationOperation::Commit) {
    result.status = network::DeviceControlStatus::Committed;
    return append_result_json(response, command.sensor_id.view(), result);
  }
  if (envelope.operation == calibration::CalibrationOperation::Start &&
      !apply_setup_fields(command, target, now_us)) return false;
  if (envelope.operation == calibration::CalibrationOperation::Preview &&
      response.status == calibration::CalibrationCommandStatus::Accepted) {
    maintenance::MaintenanceResponse preview{};
    if (!router_.append_calibration_preview_response(preview) ||
        !append_preview_json(command.sensor_id.view(), preview, result)) return false;
    result.status = network::DeviceControlStatus::PreviewReady;
    return true;
  }
  result.status = response.status == calibration::CalibrationCommandStatus::Committed
                      ? network::DeviceControlStatus::Committed
                      : response.status == calibration::CalibrationCommandStatus::Accepted
                            ? envelope.operation == calibration::CalibrationOperation::Capture
                                  ? network::DeviceControlStatus::Capturing
                                  : envelope.operation == calibration::CalibrationOperation::Validate
                                        ? network::DeviceControlStatus::Validated
                                        : network::DeviceControlStatus::Accepted
                            : network::DeviceControlStatus::Failed;
  return append_result_json(response, command.sensor_id.view(), result);
}

}  // namespace sigap

#endif  // ARDUINO
