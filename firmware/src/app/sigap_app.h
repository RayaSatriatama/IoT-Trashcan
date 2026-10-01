#pragma once

#include "config/config_manager.h"
#include "config/nvs_config_store.h"

namespace sigap {

class SigapApp final {
 public:
  SigapApp() noexcept;

  void setup() noexcept;
  void loop() noexcept;

  const config::ConfigManager& config_manager() const noexcept {
    return config_manager_;
  }

 private:
  config::NvsConfigStore config_store_{};
  config::ConfigManager config_manager_;
};

}  // namespace sigap
