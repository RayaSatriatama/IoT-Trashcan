#pragma once

#include <cstddef>
#include <cstdint>

#include "acquisition/acquisition_types.h"
#include "acquisition/sequence.h"

#ifdef ARDUINO
#include <esp_system.h>
#endif

namespace sigap::acquisition {

inline BootId make_boot_id() noexcept {
  BootId result{};
#ifdef ARDUINO
  for (std::size_t offset = 0; offset < result.bytes.size(); offset += sizeof(uint32_t)) {
    const uint32_t word = esp_random();
    for (std::size_t byte = 0; byte < sizeof(uint32_t) && offset + byte < result.bytes.size(); ++byte) {
      result.bytes[offset + byte] = static_cast<uint8_t>(word >> (byte * 8U));
    }
  }
  result.present = true;
#endif
  return result;
}

struct BootRuntimeIdentity final {
  BootId boot_id{make_boot_id()};
  SequenceAllocator sequence{};
};

}  // namespace sigap::acquisition
