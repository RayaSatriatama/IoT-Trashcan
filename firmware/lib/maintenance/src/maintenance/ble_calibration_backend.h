#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string_view>

#include "maintenance/maintenance.h"
#include "provisioning/ble_provisioning.h"

namespace sigap::maintenance {

class CommandRouter;

class ICalibrationClock {
 public:
  virtual ~ICalibrationClock() = default;
  virtual uint64_t now_us() const noexcept = 0;
};

// Bridges the existing bounded USB command grammar to the authenticated BLE
// session. It shares CommandRouter state, so USB, BLE, and future HTTPS calls
// cannot create independent calibration sessions.
class CommandRouterBleCalibrationBackend final
    : public provisioning::IBleCalibrationBackend {
 public:
  CommandRouterBleCalibrationBackend(CommandRouter& router,
                                     const ICalibrationClock& clock) noexcept
      : router_(router), clock_(clock) {}

  bool execute(std::string_view command, char* response, std::size_t capacity,
               std::size_t& response_size) noexcept override;
  void disconnect() noexcept override;

 private:
  CommandRouter& router_;
  const ICalibrationClock& clock_;
  MaintenanceParser parser_{};
  bool ble_session_owned_{false};
};

}  // namespace sigap::maintenance
