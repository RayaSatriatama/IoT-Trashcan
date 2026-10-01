#pragma once

#include "config/config_manager.h"
#include "config/nvs_config_store.h"
#include "acquisition/boot_identity.h"
#include "acquisition/record.h"
#include "acquisition/runtime.h"
#include "sigap/board/board_pins.h"

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

 private:
  config::NvsConfigStore config_store_{};
  config::ConfigManager config_manager_;
  acquisition::BootRuntimeIdentity boot_identity_{};
  acquisition::RecordContext record_context_{};
  acquisition::RecordFactory record_factory_;
  acquisition::TargetProfileSet profiles_{};
  acquisition::AcquisitionFoundation acquisition_;
};

}  // namespace sigap
