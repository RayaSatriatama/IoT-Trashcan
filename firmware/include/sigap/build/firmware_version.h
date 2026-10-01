#pragma once

#include <cstdint>

namespace sigap::build {

struct FirmwareVersion {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
};

inline constexpr FirmwareVersion kFirmwareVersion{0, 1, 0};

}  // namespace sigap::build
