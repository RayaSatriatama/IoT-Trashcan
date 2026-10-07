#pragma once

#include "config/config_manager.h"
#include "config/nvs_config_store.h"
#include "acquisition/boot_identity.h"
#include "acquisition/record.h"
#include "acquisition/runtime.h"
#include "acquisition/profile_manager.h"
#include "acquisition/profile_store.h"
#include "maintenance/runtime.h"
#include "provisioning/secret_store.h"
#include "calibration/calibration_session.h"
#include "sigap/board/board_pins.h"
#include "processing_runtime.h"
#include "models/model_lifecycle.h"
#include "maintenance/model_runtime.h"
#include "storage/archive_media.h"
#include "archive_runtime.h"

namespace sigap {

class SigapApp final {
 public:
  SigapApp() noexcept;

  void setup() noexcept;
  void loop() noexcept;

  const config::ConfigManager& config_manager() const noexcept {
    return config_manager_;
  }

  const acquisition::AcquisitionFoundation& acquisition() const noexcept {
    return acquisition_;
  }

  const maintenance::CommandRouter& maintenance_router() const noexcept {
    return maintenance_router_;
  }

 private:
  config::NvsConfigStore config_store_{};
  config::ConfigManager config_manager_;
  provisioning::NvsSecretStore secret_store_{};
  acquisition::BootRuntimeIdentity boot_identity_{};
  acquisition::RecordContext record_context_{};
  acquisition::RecordFactory record_factory_;
  calibration::CalibrationEvidenceCache evidence_cache_{};
  acquisition::TargetProfileSet profiles_{};
  acquisition::ProfileAuditRing<16U> audit_ring_{};
  acquisition::NvsProfileStore profile_store_{};
  acquisition::ProfilePersistence profile_persistence_;
  acquisition::ProfileManager profile_manager_;
  acquisition::AcquisitionFoundation acquisition_;
  ProcessingRuntime processing_runtime_{};
  storage::LittleFsArchiveStore littlefs_store_{};
  storage::SdArchiveStore sd_store_{};
  storage::MediaFailoverStore archive_store_;
  ArchiveRuntime archive_runtime_{};
  models::NvsModelProfilePersistence model_persistence_{};
  models::ModelProfileLifecycle model_lifecycle_;
  maintenance::ModelMaintenanceRouter model_router_;
  models::FisModelProfile fis_model_{};
  models::FisModelProfile fis_model_inorganic_{};
  models::FisModelProfile fis_model_b3_{};
  models::SafetyModelProfile safety_model_{};
  bool fis_model_present_{false};
  bool safety_model_present_{false};
#ifdef ARDUINO
  maintenance::ArduinoUsbTransport maintenance_transport_{};
  maintenance::ArduinoRebootController reboot_controller_{};
#else
  maintenance::NullTransport maintenance_transport_{};
  maintenance::NullRebootController reboot_controller_{};
#endif
  maintenance::CommandRouter maintenance_router_;
  maintenance::MaintenanceService maintenance_service_;
  uint32_t profile_generation_{0U};

  void refresh_model_profiles() noexcept;
};

}  // namespace sigap
