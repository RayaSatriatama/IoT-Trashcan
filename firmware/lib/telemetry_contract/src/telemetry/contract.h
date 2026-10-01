#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "telemetry/sha256.h"

namespace sigap::telemetry_contract {

inline constexpr std::size_t kMaxBodyBytes{32U * 1024U};
inline constexpr std::size_t kMaxRecordBytes{16U * 1024U};
inline constexpr uint16_t kMaxRecords{128U};
inline constexpr uint8_t kMaxJsonDepth{12U};
inline constexpr std::size_t kMaxFreeStringBytes{512U};

struct Uint64Decimal final {
  std::array<char, 20> bytes{};
  uint8_t size{0};

  bool assign(uint64_t value) noexcept {
    char reverse[20]{};
    uint8_t count{0};
    do {
      reverse[count++] = static_cast<char>('0' + (value % 10U));
      value /= 10U;
    } while (value != 0U && count < sizeof(reverse));
    if (count == 0U || count > bytes.size()) return false;
    for (uint8_t index = 0; index < count; ++index) {
      bytes[index] = reverse[count - index - 1U];
    }
    size = count;
    return true;
  }

  std::string_view view() const noexcept { return {bytes.data(), size}; }
};

inline bool is_canonical_uint64(std::string_view value) noexcept {
  if (value.empty() || (value.size() > 1U && value.front() == '0') || value.size() > 20U) return false;
  for (const char character : value) {
    if (character < '0' || character > '9') return false;
  }
  if (value.size() < 20U) return true;
  constexpr std::string_view kMaximum{"18446744073709551615"};
  return value <= kMaximum;
}

inline std::array<uint8_t, 32> record_fixture_hash(std::string_view canonical_record_without_hash) noexcept {
  return sha256(canonical_record_without_hash);
}

}  // namespace sigap::telemetry_contract
