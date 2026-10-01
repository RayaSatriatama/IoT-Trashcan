#pragma once

#include <cstdint>

#include "config/config_manager.h"
#include "acquisition/profile_manager.h"
#include "maintenance/maintenance.h"
#include "provisioning/secret_store.h"
#include "calibration/calibration_session.h"

namespace sigap::maintenance {

enum class DeviceMode : uint8_t {
  Run,
  Maintenance,
  Calibrating,
  ConfigRequired,
  RebootRequired,
  StorageError,
};

class NullTransport final : public IMaintenanceTransport {
 public:
  bool available() noexcept override { return false; }
  int read() noexcept override { return -1; }
  bool write(std::string_view) noexcept override { return true; }
};

#ifdef ARDUINO
class ArduinoUsbTransport final : public IMaintenanceTransport {
 public:
  bool available() noexcept override;
  int read() noexcept override;
  bool write(std::string_view value) noexcept override;
};
#endif

class CommandRouter final : public IMaintenanceCommandSink {
 public:
  CommandRouter(config::ConfigManager& config,
                provisioning::SecretStore& secrets,
                acquisition::ProfileManager* profiles = nullptr) noexcept
      : config_(config), secrets_(secrets), profiles_(profiles) {
    refresh_mode();
  }

  CommandStatus execute(const MaintenanceCommand& command,
                        uint64_t now_us,
                        MaintenanceResponse& response) noexcept override;

  void refresh_mode() noexcept;
  DeviceMode mode() const noexcept { return mode_; }

 private:
  static bool field_from_text(const FixedText<64U>& text,
                              config::ConfigField& field) noexcept;
  static bool append_u64(MaintenanceResponse& response,
                         const char* label, uint64_t value) noexcept;
  static bool append_hash(MaintenanceResponse& response,
                          const config::ConfigHash& hash) noexcept;
  static bool calibration_target_from_text(
      const FixedText<64U>& text,
      calibration::CalibrationTarget& target) noexcept;
  CommandStatus execute_config(const MaintenanceCommand& command,
                               MaintenanceResponse& response) noexcept;

  config::ConfigManager& config_;
  provisioning::SecretStore& secrets_;
  acquisition::ProfileManager* profiles_{nullptr};
  calibration::CalibrationSession calibration_{};
  DeviceMode mode_{DeviceMode::ConfigRequired};
};

}  // namespace sigap::maintenance
