#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "maintenance.h"

namespace sigap::maintenance {

// Compact keys keep all diagnostics and final status inside the 192-byte
// response, even when all four numeric diagnostics reach UINT64_MAX.
// bus is mmc1 or spi; n is bytes; us is elapsed microseconds;
// t/u/f are total/used/free capacity in bytes.
inline CommandStatus append_sd_probe_pass(
    MaintenanceResponse& response, std::string_view bus,
    uint16_t bytes, const std::array<uint8_t, 32U>& hash,
    uint64_t elapsed_us, uint64_t total_bytes, uint64_t used_bytes,
    uint64_t free_bytes, bool archive_verified = false) noexcept {
  if (!response.empty() || (bus != "mmc1" && bus != "spi") || bytes > 256U)
    return CommandStatus::Failed;
  char number[32U]{};
  const auto append_u64 = [&response, &number](
      const char* label, uint64_t value) noexcept {
    const int size = std::snprintf(number, sizeof(number), "%llu",
                                  static_cast<unsigned long long>(value));
    return size > 0 && static_cast<std::size_t>(size) < sizeof(number) &&
           response.append(label) &&
           response.append(std::string_view(number, static_cast<std::size_t>(size))) &&
           response.append("\n");
  };
  if (!response.append("sd=PASS\nbus=") || !response.append(bus) ||
      !response.append("\n") || !append_u64("n=", bytes) ||
      !response.append("hash="))
    return CommandStatus::Failed;
  constexpr char digits[] = "0123456789abcdef";
  for (const uint8_t byte : hash) {
    const char pair[2U]{digits[byte >> 4U], digits[byte & 0x0FU]};
    if (!response.append(std::string_view(pair, sizeof(pair))))
      return CommandStatus::Failed;
  }
  return response.append("\n") && append_u64("us=", elapsed_us) &&
                 append_u64("t=", total_bytes) && append_u64("u=", used_bytes) &&
                 append_u64("f=", free_bytes) &&
                 (!archive_verified || response.append("a=PASS\n")) &&
                 response.append_status(CommandStatus::Accepted)
             ? CommandStatus::Accepted : CommandStatus::Failed;
}

}  // namespace sigap::maintenance
