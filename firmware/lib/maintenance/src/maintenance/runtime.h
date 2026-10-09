#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

#include "config/config_manager.h"
#include "acquisition/profile_manager.h"
#include "maintenance/maintenance.h"
#include "provisioning/secret_store.h"
#include "provisioning/ble_provisioning.h"
#include "calibration/calibration_session.h"
#include "calibration/calibration_transport.h"

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
                            public acquisition::ICalibrationStateProvider,
                            public calibration::ICalibrationTransportWorkflow {
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

  // Typed calibration transports call through this boundary so the USB
  // maintenance guard cannot reject the controller's own workflow calls.
  CommandStatus execute_calibration_internal(
      const MaintenanceCommand& command, uint64_t now_us,
      MaintenanceResponse& response) noexcept;

  void set_calibration_transport_controller(
      calibration::CalibrationTransportController* controller) noexcept {
    calibration_controller_ = controller;
  }

  void refresh_mode() noexcept;
  DeviceMode mode() const noexcept { return mode_; }
  bool is_calibrating(acquisition::SensorId sensor) const noexcept override;
  bool reboot_requested() const noexcept { return reboot_requested_; }
  void poll(uint64_t now_us) noexcept override;
  void transport_lost() noexcept override;

  // Typed transport workflow used by the shared USB/BLE/HTTPS controller.
  bool status(calibration::CalibrationStatusSnapshot& output) const noexcept override;
  bool evidence(calibration::CalibrationTarget target,
                calibration::DiagnosticEvidence& output) const noexcept override;
  bool active_profile(calibration::CalibrationTarget target,
                      calibration::CalibrationProfileRef& output) const noexcept override;
  bool proposed_profile(
      calibration::CalibrationProfileRef& output) const noexcept override;
  bool start(calibration::CalibrationTarget target, uint64_t now_us) noexcept override;
  bool set_criteria(const calibration::CaptureCriteria& criteria,
                    uint64_t now_us) noexcept override;
  bool capture(calibration::CalibrationAnchor anchor,
               uint64_t now_us) noexcept override;
  bool validate() noexcept override;
  bool preview(calibration::ValidationResult& output) noexcept override;
  bool commit() noexcept override;
  bool cancel() noexcept override;
  bool history(calibration::CalibrationResultEnvelope& output) const noexcept override;
  bool rollback() noexcept override;
  bool append_calibration_preview_response(
      MaintenanceResponse& response) const noexcept;
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
  struct CalibrationProposal final {
    bool present{false};
    acquisition::ProfileKey key{};
    acquisition::EncodedProfile encoded{};
    acquisition::ActiveProfileSnapshot active{};
    uint64_t created_at_us{0U};
    uint64_t evidence_sequence{0U};
  };
  bool build_calibration_proposal(uint64_t now_us,
                                  CalibrationProposal& proposal) const noexcept;
  bool calibration_proposal_current() const noexcept;
  bool append_calibration_preview(MaintenanceResponse& response) const noexcept;
  void clear_calibration_proposal() noexcept { calibration_proposal_ = CalibrationProposal{}; }
  CommandStatus execute_config(const MaintenanceCommand& command,
                               MaintenanceResponse& response) noexcept;
  CommandStatus sensor_status(const MaintenanceCommand& command,
                              uint64_t now_us,
                              MaintenanceResponse& response) noexcept;
  bool commit_calibration_proposal() noexcept;
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
  calibration::CalibrationTransportController* calibration_controller_{nullptr};
  bool internal_calibration_dispatch_{false};
  calibration::CalibrationSession calibration_{};
  CalibrationProposal calibration_proposal_{};
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
  // First-time sensor setup parameters are explicit maintenance criteria.
  // They are session-owned and are never persisted independently of the
  // validated profile produced at commit.
  uint32_t ultrasonic_echo_mm_per_us_milli_{0U};
  uint32_t ultrasonic_echo_timeout_us_{0U};
  uint32_t ultrasonic_inter_sensor_gap_us_{0U};
  uint32_t ultrasonic_overshoot_tolerance_mm_milli_{0U};
  bool ultrasonic_echo_conversion_set_{false};
  bool ultrasonic_tolerance_set_{false};
  bool ultrasonic_timing_set_{false};
  uint32_t mq_reference_mv_{0U};
  uint32_t mq_full_scale_counts_{0U};
  uint32_t mq_divider_numerator_{0U};
  uint32_t mq_divider_denominator_{0U};
  bool mq_engineering_set_{false};
  uint8_t ds_resolution_bits_{12U};
  bool ds_resolution_set_{false};
  std::array<uint8_t, 8U> ds_rom_{};
  bool ds_rom_set_{false};
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
