#include "maintenance/ble_calibration_backend.h"
#include "maintenance/runtime.h"

#include <algorithm>
#include <charconv>

namespace sigap::maintenance {
namespace {

bool append_text(char* output, std::size_t capacity, std::size_t& size,
                 std::string_view value) noexcept {
  if (size + value.size() >= capacity) return false;
  for (std::size_t index = 0U; index < value.size(); ++index) {
    output[size + index] = value[index];
  }
  size += value.size();
  output[size] = '\0';
  return true;
}

bool append_escaped_data(char* output, std::size_t capacity, std::size_t& size,
                         std::string_view value) noexcept {
  for (const char byte : value) {
    // Maintenance responses are line-oriented.  Keep the BLE envelope
    // single-line and deterministic while retaining every field/value.
    if (byte == '|' ) {
      if (!append_text(output, capacity, size, "%7C")) return false;
      continue;
    }
    const char replacement = byte == '\n' || byte == '\r' ? ';' : byte;
    if (!append_text(output, capacity, size, std::string_view(&replacement, 1U))) return false;
  }
  return true;
}

std::string_view operation_name(std::string_view operation) noexcept {
  if (operation == "ACTIVE_PROFILE") return "PROFILE";
  return operation;
}

bool sensor_from_arguments(std::string_view arguments,
                           std::string_view& sensor) noexcept {
  const auto separator = arguments.find(' ');
  sensor = separator == std::string_view::npos
               ? arguments
               : arguments.substr(0U, separator);
  return !sensor.empty();
}

bool append_envelope(char* output, std::size_t capacity, std::size_t& size,
                     std::string_view operation, bool accepted,
                     std::string_view sensor, uint64_t session_id,
                     std::string_view payload) noexcept {
  size = 0U;
  if (!append_text(output, capacity, size, "CAL1|op=") ||
      !append_text(output, capacity, size, operation_name(operation)) ||
      !append_text(output, capacity, size, "|status=") ||
      !append_text(output, capacity, size, accepted ? "ACCEPTED" : "ERROR") ||
      !append_text(output, capacity, size, "|session=") ) return false;
  char session[21]{};
  const auto result = std::to_chars(session, session + sizeof(session), session_id);
  if (result.ec != std::errc{} ||
      !append_text(output, capacity, size,
                   std::string_view(session, static_cast<std::size_t>(result.ptr - session))) ||
      !append_text(output, capacity, size, "|sensor=") ||
      !append_text(output, capacity, size, sensor) ||
      !append_text(output, capacity, size, "|data=") ||
      !append_escaped_data(output, capacity, size, payload)) return false;
  return size < capacity && (output[size] = '\0', true);
}

bool append_error_envelope(char* output, std::size_t capacity,
                           std::size_t& size, std::string_view operation,
                           std::string_view sensor, uint64_t session_id,
                           std::string_view code) noexcept {
  return append_envelope(output, capacity, size, operation, false, sensor,
                         session_id, code);
}

bool capability_data(std::string_view sensor, std::string_view& data) noexcept {
  if (sensor == "hx711_organic" || sensor == "hx711_inorganic" ||
      sensor == "hx711_b3") {
    data = "references=tare_counts:counts,reference_mass_mg:mg;criteria=samples:count,max_span_counts:counts";
    return true;
  }
  if (sensor == "ultrasonic_organic" || sensor == "ultrasonic_inorganic" ||
      sensor == "ultrasonic_b3") {
    data = "references=empty_mm:mm,full_mm:mm;criteria=samples:count,max_span_mm:mm;engineering=echo_mm_per_us_milli:mm_per_us,echo_timeout_us:us,inter_sensor_gap_us:us,overshoot_tolerance_mm_milli:mm";
    return true;
  }
  if (sensor == "mq4" || sensor == "mq135" || sensor == "mq7" ||
      sensor == "mq8") {
    data = "references=none;criteria=samples:count,max_span_counts:counts;engineering=reference_mv:mV,full_scale_counts:counts,divider_numerator:ratio,divider_denominator:ratio";
    return true;
  }
  if (sensor == "ds18b20_b3") {
    data = "references=reference_temperature_centi_c:centi_celsius;criteria=samples:count,max_span_centi_c:centi_celsius;engineering=resolution_bits:bits";
    return true;
  }
  if (sensor == "sht31_44" || sensor == "sht31_45") {
    data = "references=reference_temperature_centi_c:centi_celsius,reference_humidity_centi_pct:centi_percent;criteria=samples:count,max_temperature_span_centi_c:centi_celsius,max_humidity_span_centi_pct:centi_percent;engineering=address:i2c";
    return true;
  }
  return false;
}

bool dispatch_line(MaintenanceParser& parser, CommandRouter& router,
                   const ICalibrationClock& clock, std::string_view line,
                   MaintenanceResponse& response,
                   CommandKind& kind) noexcept {
  parser.reset();
  ParseEvent event{};
  for (const char byte : line) {
    if (parser.consume(byte, event)) return false;
  }
  if (!parser.consume('\n', event) || event.status != CommandStatus::Accepted) {
    return false;
  }
  kind = event.command.kind;
  response = MaintenanceResponse{};
  return router.execute(event.command, clock.now_us(), response) ==
         CommandStatus::Accepted;
}

void copy_response(const MaintenanceResponse& source, char* output,
                   std::size_t capacity, std::size_t& size) noexcept {
  size = std::min(capacity, source.size());
  for (std::size_t index = 0U; index < size; ++index) {
    output[index] = source.view()[index];
  }
}

bool active_sensor(CommandRouter& router, std::string_view value) noexcept {
  struct Entry {
    std::string_view name;
    acquisition::SensorId sensor;
  };
  constexpr Entry entries[] = {
      {"hx711_organic", acquisition::SensorId::Hx711Organic},
      {"hx711_inorganic", acquisition::SensorId::Hx711Inorganic},
      {"hx711_b3", acquisition::SensorId::Hx711B3},
      {"ultrasonic_organic", acquisition::SensorId::UltrasonicOrganic},
      {"ultrasonic_inorganic", acquisition::SensorId::UltrasonicInorganic},
      {"ultrasonic_b3", acquisition::SensorId::UltrasonicB3},
      {"mq4", acquisition::SensorId::Mq4},
      {"mq135", acquisition::SensorId::Mq135},
      {"mq7", acquisition::SensorId::Mq7},
      {"mq8", acquisition::SensorId::Mq8},
      {"ds18b20_b3", acquisition::SensorId::Ds18b20B3},
      {"sht31_44", acquisition::SensorId::Sht31_44},
      {"sht31_45", acquisition::SensorId::Sht31_45},
  };
  for (const auto& entry : entries) {
    if (entry.name == value) return router.is_calibrating(entry.sensor);
  }
  return false;
}

}  // namespace

bool CommandRouterBleCalibrationBackend::execute(
    std::string_view command, char* response, std::size_t capacity,
    std::size_t& response_size) noexcept {
  response_size = 0U;
  if (response == nullptr || capacity == 0U ||
      command.size() >= kMaxCommandLineBytes ||
      command.substr(0U, 4U) != "CAL ") {
    return false;
  }

  command.remove_prefix(4U);
  const auto separator = command.find(' ');
  const auto operation = command.substr(0U, separator);
  const auto arguments = separator == std::string_view::npos
                             ? std::string_view{}
                             : command.substr(separator + 1U);
  constexpr std::string_view prefix{"calibration "};

  std::string_view sensor{};
  (void)sensor_from_arguments(arguments, sensor);

  if (operation == "CAPABILITIES") {
    std::string_view data{};
    if (!capability_data(sensor, data)) {
      (void)append_error_envelope(response, capacity, response_size, operation,
                                  sensor, session_id_, "INVALID_SENSOR");
      return false;
    }
    return append_envelope(response, capacity, response_size, operation, true,
                           sensor, session_id_, data);
  }

  if (operation == "HISTORY" || operation == "ROLLBACK") {
    // No history/rollback primitive is exposed by ProfileManager yet.  Keep
    // this explicit and typed instead of fabricating a successful result.
    (void)append_error_envelope(response, capacity, response_size, operation,
                                sensor, session_id_, "UNSUPPORTED");
    return false;
  }

  if (operation == "ACTIVE_PROFILE") {
    if (sensor.empty()) {
      (void)append_error_envelope(response, capacity, response_size, operation,
                                  sensor, session_id_, "INVALID_SENSOR");
      return false;
    }
    std::array<char, kMaxCommandLineBytes> line{};
    std::size_t line_size{0U};
    if (!append_text(line.data(), line.size(), line_size, "profile status ") ||
        !append_text(line.data(), line.size(), line_size, sensor)) {
      return false;
    }
    MaintenanceResponse maintenance_response{};
    CommandKind kind{CommandKind::Unknown};
    const bool accepted = dispatch_line(parser_, router_, clock_,
                                        std::string_view(line.data(), line_size),
                                        maintenance_response, kind);
    const bool encoded = append_envelope(response, capacity, response_size,
                                         operation, accepted, sensor,
                                         session_id_, maintenance_response.view());
    return accepted && encoded;
  }

  if (operation == "STATUS" || operation == "EVIDENCE") {
    std::array<char, kMaxCommandLineBytes> line{};
    std::size_t line_size{0U};
    if (arguments.empty() && operation == "STATUS") {
      if (!append_text(line.data(), line.size(), line_size, "calibration status")) {
        return false;
      }
    } else if (arguments.empty() ||
               !append_text(line.data(), line.size(), line_size, "sensor status ") ||
               !append_text(line.data(), line.size(), line_size, arguments)) {
      return false;
    }
    MaintenanceResponse maintenance_response{};
    CommandKind kind{CommandKind::Unknown};
    const bool accepted = dispatch_line(
        parser_, router_, clock_, std::string_view(line.data(), line_size),
        maintenance_response, kind);
    const bool encoded = append_envelope(response, capacity, response_size,
                                         operation, accepted, sensor,
                                         session_id_, maintenance_response.view());
    return accepted && encoded;
  }

  if (operation == "CRITERIA") {
    const auto sensor_end = arguments.find(' ');
    if (sensor_end == std::string_view::npos || sensor_end == 0U) return false;
    if (!active_sensor(router_, arguments.substr(0U, sensor_end))) return false;
    const auto values = arguments.substr(sensor_end + 1U);
    if (values.empty()) return false;
    std::size_t cursor{0U};
    bool dispatched{false};
    MaintenanceResponse last_response{};
    while (cursor < values.size()) {
      const auto end = values.find(' ', cursor);
      const auto token = values.substr(
          cursor, end == std::string_view::npos ? values.size() - cursor
                                                : end - cursor);
      const auto equals = token.find('=');
      if (equals == std::string_view::npos || equals == 0U ||
          equals + 1U >= token.size()) return false;
      std::array<char, kMaxCommandLineBytes> line{};
      std::size_t line_size{0U};
      if (!append_text(line.data(), line.size(), line_size, prefix) ||
          !append_text(line.data(), line.size(), line_size, "criteria ") ||
          !append_text(line.data(), line.size(), line_size,
                       token.substr(0U, equals)) ||
          !append_text(line.data(), line.size(), line_size, " ") ||
          !append_text(line.data(), line.size(), line_size,
                       token.substr(equals + 1U))) {
        return false;
      }
      CommandKind kind{CommandKind::Unknown};
      if (!dispatch_line(parser_, router_, clock_,
                         std::string_view(line.data(), line_size), last_response,
                         kind)) {
        return false;
      }
      dispatched = true;
      if (end == std::string_view::npos) break;
      cursor = end + 1U;
    }
    if (!dispatched) return false;
    return append_envelope(response, capacity, response_size, operation, true,
                           sensor, session_id_,
                           last_response.view());
  }

  std::array<char, kMaxCommandLineBytes> line{};
  std::size_t line_size{0U};
  if (!append_text(line.data(), line.size(), line_size, prefix)) return false;
  if (operation == "START") {
    if (arguments.empty() ||
        !append_text(line.data(), line.size(), line_size, "start ") ||
        !append_text(line.data(), line.size(), line_size, arguments)) return false;
  } else if (operation == "CAPTURE") {
    const auto sensor_end = arguments.find(' ');
    if (sensor_end == std::string_view::npos ||
        !active_sensor(router_, arguments.substr(0U, sensor_end))) return false;
    const auto anchor = arguments.substr(sensor_end + 1U);
    if (anchor.empty() || anchor.find(' ') != std::string_view::npos ||
        (anchor != "tare" && anchor != "reference" && anchor != "empty" &&
         anchor != "full")) return false;
    if (!append_text(line.data(), line.size(), line_size, "capture ") ||
        !append_text(line.data(), line.size(), line_size, anchor)) return false;
  } else if (operation == "VALIDATE" || operation == "PREVIEW" ||
             operation == "COMMIT") {
    if (arguments.empty() || !active_sensor(router_, arguments) ||
        !append_text(line.data(), line.size(), line_size,
                     operation == "VALIDATE" ? "validate" :
                     operation == "PREVIEW" ? "preview" : "commit")) {
      return false;
    }
  } else if (operation == "CANCEL") {
    if (!arguments.empty() ||
        !append_text(line.data(), line.size(), line_size, "cancel")) return false;
  } else {
    return false;
  }

  MaintenanceResponse maintenance_response{};
  CommandKind kind{CommandKind::Unknown};
  const bool accepted = dispatch_line(
      parser_, router_, clock_, std::string_view(line.data(), line_size),
      maintenance_response, kind);
  const uint64_t response_session = session_id_;
  if (accepted) {
    if (kind == CommandKind::CalibrationStart) {
      ble_session_owned_ = true;
      session_id_ = next_session_id_++;
      session_sensor_.fill('\0');
      const auto copied = std::min(arguments.size(), session_sensor_.size() - 1U);
      std::copy_n(arguments.data(), copied, session_sensor_.data());
      session_sensor_[copied] = '\0';
    } else if (kind == CommandKind::CalibrationCancel ||
               kind == CommandKind::CalibrationCommit) {
      ble_session_owned_ = false;
    }
  }
  const bool encoded = append_envelope(
      response, capacity, response_size, operation, accepted, sensor,
      kind == CommandKind::CalibrationStart ? session_id_ : response_session,
      maintenance_response.view());
  if (accepted && (kind == CommandKind::CalibrationCancel ||
                   kind == CommandKind::CalibrationCommit)) {
    session_id_ = 0U;
    session_sensor_.fill('\0');
  }
  return accepted && encoded;
}

void CommandRouterBleCalibrationBackend::disconnect() noexcept {
  parser_.reset();
  if (ble_session_owned_) {
    router_.transport_lost();
    ble_session_owned_ = false;
  }
  session_id_ = 0U;
  session_sensor_.fill('\0');
}

}  // namespace sigap::maintenance
