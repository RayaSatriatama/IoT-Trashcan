#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string_view>

#include "maintenance/maintenance.h"
#include "provisioning/ble_provisioning.h"
#include "calibration/calibration_transport.h"
#include "debug/io_gate.h"

namespace sigap::maintenance {

class CommandRouter;

class ICalibrationClock {
 public:
  virtual ~ICalibrationClock() = default;
  virtual uint64_t now_us() const noexcept = 0;
};

// Bridges the existing bounded USB command grammar to the no-auth BLE
// prototype session. It shares CommandRouter state, so USB, BLE, and future
// HTTPS calls cannot create independent calibration sessions.
class CommandRouterBleCalibrationBackend final
    : public provisioning::IBleCalibrationBackend {
 public:
  CommandRouterBleCalibrationBackend(CommandRouter& router,
                                     const ICalibrationClock& clock,
                                     calibration::CalibrationTransportController* controller = nullptr) noexcept
      : router_(router), clock_(clock), controller_(controller),
        authorizer_(controller) {}

  bool execute(std::string_view command, char* response, std::size_t capacity,
               std::size_t& response_size) noexcept override;
  void disconnect() noexcept override;
  void set_io_gate(debug::RuntimeIoGate* gate) noexcept { io_gate_ = gate; }

 private:
  class BleAuthorizer final
      : public calibration::ICalibrationTransportAuthorizer {
   public:
    explicit BleAuthorizer(
        calibration::CalibrationTransportController* controller) noexcept
        : controller_(controller) {}
    bool authorize(
        const calibration::CalibrationCommandEnvelope& command) const noexcept override {
      return controller_ != nullptr &&
             command.transport == calibration::CalibrationTransport::Ble &&
             command.device_id == controller_->device_id();
    }

   private:
    calibration::CalibrationTransportController* controller_{nullptr};
  };

  bool execute_controller(std::string_view operation,
                          std::string_view arguments,
                          char* response, std::size_t capacity,
                          std::size_t& response_size) noexcept;
  CommandRouter& router_;
  const ICalibrationClock& clock_;
  calibration::CalibrationTransportController* controller_{nullptr};
  BleAuthorizer authorizer_;
  debug::RuntimeIoGate* io_gate_{nullptr};
  MaintenanceParser parser_{};
  bool ble_session_owned_{false};
  uint64_t session_id_{0U};
  uint64_t next_session_id_{1U};
  std::array<char, 64U> session_sensor_{};
  uint64_t next_request_id_{1U};
};

}  // namespace sigap::maintenance
