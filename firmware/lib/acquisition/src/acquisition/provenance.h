#pragma once

#include "acquisition/acquisition_types.h"
#include "config/config_manager.h"

namespace sigap::acquisition {

inline ConfigRef active_config_ref(const config::ConfigManager& manager) noexcept {
  const auto& identity = manager.identity();
  return ConfigRef{identity.schema_version, identity.config_version, identity.hash,
                   manager.runtime_ready() && identity.has_slot};
}

}  // namespace sigap::acquisition
