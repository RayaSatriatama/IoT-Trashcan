#pragma once

#ifdef ARDUINO

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "calibration/calibration_transport.h"
#include "maintenance/runtime.h"
#include "network/device_control.h"
#include "debug/io_gate.h"

namespace sigap {

class ApplicationCalibrationAuthorizer final
    : public calibration::ICalibrationTransportAuthorizer {
 public:
  void set_device_id(uint64_t value) noexcept { device_id_ = value; }

  bool authorize(const calibration::CalibrationCommandEnvelope& command) const noexcept override {
    return device_id_ != 0U && command.device_id == device_id_ &&
           command.transport != calibration::CalibrationTransport::Usb;
  }

 private:
  uint64_t device_id_{0U};
};

class ApplicationCalibrationExecutor final
    : public network::IDeviceControlExecutor {
 public:
  ApplicationCalibrationExecutor(
      maintenance::CommandRouter& router,
      calibration::CalibrationTransportController& controller) noexcept
      : router_(router), controller_(controller) {}

  bool execute(const network::DeviceControlCommand& command,
               uint64_t now_us,
               network::DeviceControlResult& result) noexcept override;
  void set_io_gate(debug::RuntimeIoGate* gate) noexcept { io_gate_ = gate; }

 private:
  static bool sensor_target(std::string_view value,
                            calibration::CalibrationTarget& output) noexcept;
  static uint64_t stable_id(std::string_view value) noexcept;
  static bool json_number(std::string_view payload, std::string_view key,
                          int64_t& output) noexcept;
  static bool json_string(std::string_view payload, std::string_view key,
                          std::string_view& output) noexcept;
  static bool digest_payload(const network::DeviceControlCommand& command,
                             calibration::CalibrationCommandEnvelope& envelope) noexcept;
  bool dispatch_line(std::string_view line, uint64_t now_us,
                     maintenance::MaintenanceResponse& response) noexcept;
  static bool append_result_json(const calibration::CalibrationResultEnvelope& source,
                                 std::string_view sensor,
                                 network::DeviceControlResult& output) noexcept;
  static bool append_preview_json(std::string_view sensor,
                                  const maintenance::MaintenanceResponse& preview,
                                  network::DeviceControlResult& output) noexcept;
  bool apply_setup_fields(const network::DeviceControlCommand& command,
                          calibration::CalibrationTarget target,
                          uint64_t now_us, bool apply) noexcept;

  maintenance::CommandRouter& router_;
  calibration::CalibrationTransportController& controller_;
  debug::RuntimeIoGate* io_gate_{nullptr};
  maintenance::MaintenanceParser parser_{};
};

}  // namespace sigap

#endif  // ARDUINO
