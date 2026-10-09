#include "maintenance/ble_calibration_backend.h"
#include "maintenance/runtime.h"

#include <algorithm>

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

  if (operation == "CAPABILITIES" || operation == "HISTORY" ||
      operation == "ROLLBACK") {
    // These operations need a typed workflow API that is not exposed by the
    // legacy maintenance router. Rejecting them is safer than fabricating a
    // capability/history/rollback response.
    return false;
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
    copy_response(maintenance_response, response, capacity, response_size);
    return accepted;
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
    copy_response(last_response, response, capacity, response_size);
    return true;
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
  if (accepted) {
    if (kind == CommandKind::CalibrationStart) {
      ble_session_owned_ = true;
    } else if (kind == CommandKind::CalibrationCancel ||
               kind == CommandKind::CalibrationCommit) {
      ble_session_owned_ = false;
    }
  }
  copy_response(maintenance_response, response, capacity, response_size);
  return accepted;
}

void CommandRouterBleCalibrationBackend::disconnect() noexcept {
  parser_.reset();
  if (ble_session_owned_) {
    router_.transport_lost();
    ble_session_owned_ = false;
  }
}

}  // namespace sigap::maintenance
