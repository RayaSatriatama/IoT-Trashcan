#pragma once

#include <cstddef>
#include <cstdint>

#include "config/config_manager.h"
#include "acquisition/profile_manager.h"
#include "maintenance/maintenance.h"
#include "provisioning/secret_store.h"
#include "provisioning/ble_provisioning.h"
#include "calibration/calibration_session.h"

namespace sigap::maintenance {

class ModelMaintenanceRouter;

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

class IRebootController {
 public:
  virtual ~IRebootController() = default;
  virtual void request_reboot() noexcept = 0;
};

#ifdef ARDUINO
class ArduinoUsbTransport final : public IMaintenanceTransport {
 public:
  bool available() noexcept override;
  int read() noexcept override;
  bool write(std::string_view value) noexcept override;
  bool connected() noexcept override;
};

class ArduinoRebootController final : public IRebootController {
 public:
  void request_reboot() noexcept override;
  bool consume_request() noexcept {
    const bool requested = requested_;
    requested_ = false;
    return requested;
  }

 private:
  bool requested_{false};
};
#endif

class NullRebootController final : public IRebootController {
 public:
  void request_reboot() noexcept override {}
  bool consume_request() noexcept { return false; }
};

class CommandRouter final : public IMaintenanceCommandSink,
                            public acquisition::ICalibrationStateProvider {
 public:
  static constexpr uint64_t kTransferTimeoutUs{300000000ULL};

  CommandRouter(config::ConfigManager& config,
                provisioning::SecretStore& secrets,
                acquisition::ProfileManager* profiles = nullptr,
                calibration::ICalibrationEvidenceSource* evidence = nullptr,
                IRebootController* reboot = nullptr,
                acquisition::IProfileAuditSink* audit = nullptr,
                const acquisition::BootId* boot_id = nullptr,
                IModelCommandHandler* models = nullptr,
                IStorageTestProbe* storage_probe = nullptr,
                provisioning::ILocalPasskeyHandoff* passkey_handoff = nullptr) noexcept
      : config_(config), secrets_(secrets), profiles_(profiles),
        evidence_(evidence), reboot_(reboot), audit_(audit), boot_id_(boot_id),
        models_(models), storage_probe_(storage_probe),
        passkey_handoff_(passkey_handoff) {
    refresh_mode();
  }

  CommandStatus execute(const MaintenanceCommand& command,
                        uint64_t now_us,
                        MaintenanceResponse& response) noexcept override;

  void refresh_mode() noexcept;
  DeviceMode mode() const noexcept { return mode_; }
  bool is_calibrating(acquisition::SensorId sensor) const noexcept override;
  bool reboot_requested() const noexcept { return reboot_requested_; }
  void poll(uint64_t now_us) noexcept override;
  void transport_lost() noexcept override;
  void set_passkey_handoff(
      provisioning::ILocalPasskeyHandoff* handoff) noexcept {
    passkey_handoff_ = handoff;
  }

 private:
  static bool field_from_text(const FixedText<64U>& text,
                              config::ConfigField& field) noexcept;
  static bool append_u64(MaintenanceResponse& response,
                         const char* label, uint64_t value,
                         bool newline = true) noexcept;
  static bool append_hash(MaintenanceResponse& response,
                          const config::ConfigHash& hash) noexcept;
  static bool calibration_target_from_text(
      const FixedText<64U>& text,
      calibration::CalibrationTarget& target) noexcept;
  CommandStatus execute_config(const MaintenanceCommand& command,
                               MaintenanceResponse& response) noexcept;
  bool build_and_stage_calibration_profile(uint64_t now_us) noexcept;
  CommandStatus profile_transfer(const MaintenanceCommand& command,
                                 MaintenanceResponse& response) noexcept;
  void cancel_profile_transfer() noexcept;

  config::ConfigManager& config_;
  provisioning::SecretStore& secrets_;
  acquisition::ProfileManager* profiles_{nullptr};
  calibration::ICalibrationEvidenceSource* evidence_{nullptr};
  IRebootController* reboot_{nullptr};
  acquisition::IProfileAuditSink* audit_{nullptr};
  const acquisition::BootId* boot_id_{nullptr};
  IModelCommandHandler* models_{nullptr};
  IStorageTestProbe* storage_probe_{nullptr};
  provisioning::ILocalPasskeyHandoff* passkey_handoff_{nullptr};
  calibration::CalibrationSession calibration_{};
  uint64_t evidence_sequence_{0U};
  bool criteria_set_{false};
  calibration::CalibrationTarget pending_target_{calibration::CalibrationTarget::Hx711Organic};
  uint16_t criteria_samples_{0U};
  int64_t criteria_span_{0};
  bool criteria_span_set_{false};
  int32_t criteria_temperature_span_{0};
  int32_t criteria_humidity_span_{0};
  bool criteria_temperature_span_set_{false};
  bool criteria_humidity_span_set_{false};
  uint64_t reference_mass_mg_{0U};
  int32_t reference_temperature_centi_c_{0};
  int32_t reference_humidity_centi_pct_{0};
  bool reference_temperature_set_{false};
  bool reference_humidity_set_{false};
  bool reboot_requested_{false};
  bool maintenance_authorized_{false};
  acquisition::ProfileKey transfer_key_{
      acquisition::ProfileKind::Calibration,
      acquisition::ProfileTarget::Unknown};
  acquisition::EncodedProfile transfer_profile_{};
  std::size_t transfer_expected_size_{0U};
  std::size_t transfer_received_size_{0U};
  std::size_t transfer_export_offset_{0U};
  config::ConfigHash transfer_expected_hash_{};
  bool transfer_expected_hash_present_{false};
  bool transfer_import_active_{false};
  bool transfer_export_active_{false};
  bool transfer_draft_active_{false};
  uint64_t transfer_last_activity_us_{0U};
  DeviceMode mode_{DeviceMode::ConfigRequired};
};

}  // namespace sigap::maintenance
