#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace sigap::storage {

// The probe path is deterministic for one device, owned by the probe, and
// distinct from the historical Task 6 filename. Callers must reject an
// existing path before opening it for writing.
inline bool build_sd_probe_path(uint64_t device_id, char* output,
                                std::size_t capacity) noexcept {
  if (output == nullptr || capacity == 0U) return false;
  const int written = std::snprintf(
      output, capacity, "/sigap/task9_sd_probe_%08llx.bin",
      static_cast<unsigned long long>(device_id & 0xffffffffULL));
  return written > 0 && static_cast<std::size_t>(written) < capacity;
}

}  // namespace sigap::storage
