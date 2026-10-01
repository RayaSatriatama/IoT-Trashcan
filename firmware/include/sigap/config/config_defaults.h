#pragma once

#include "sigap/build/build_config.h"
#include "config/config_types.h"

namespace sigap::config {

inline constexpr AppConfig kFactoryDefaults{
    build::kConfigSchemaVersion,
    0,
    SamplingConfig{100, 100, 1000, 1000, 2000},
    TelemetryConfig{15000, 128, 1000, 60000},
    StorageConfig{6ULL * 1024ULL * 1024ULL,
                  20ULL * 1024ULL * 1024ULL * 1024ULL},
};

}  // namespace sigap::config
