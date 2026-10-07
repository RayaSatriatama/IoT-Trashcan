#pragma once

#include <cstdint>

namespace sigap::board {

inline constexpr uint64_t kExpectedFlashBytes{16ULL * 1024ULL * 1024ULL};
inline constexpr uint64_t kExpectedPsramBytes{8ULL * 1024ULL * 1024ULL};

enum class BoardMemoryStatus : uint8_t {
  Match,
  FlashSizeMismatch,
  PsramMissing,
  PsramSizeMismatch,
};

struct BoardMemoryReading {
  uint64_t flash_bytes{0};
  bool psram_present{false};
  // Physical chip capacity used for N16R8 identity acceptance. This must not
  // be replaced with an allocator metric: heap reservations and allocations
  // can make the available PSRAM smaller than the attached chip.
  uint64_t psram_physical_bytes{0};
  // Runtime allocator diagnostics. These are intentionally not used by
  // evaluate_board_memory().
  uint64_t psram_heap_total_bytes{0};
  uint64_t psram_heap_free_bytes{0};
  uint64_t psram_heap_largest_free_block_bytes{0};
};

constexpr BoardMemoryStatus evaluate_board_memory(
    const BoardMemoryReading& reading) noexcept {
  if (reading.flash_bytes != kExpectedFlashBytes) {
    return BoardMemoryStatus::FlashSizeMismatch;
  }
  if (!reading.psram_present) {
    return BoardMemoryStatus::PsramMissing;
  }
  if (reading.psram_physical_bytes != kExpectedPsramBytes) {
    return BoardMemoryStatus::PsramSizeMismatch;
  }
  return BoardMemoryStatus::Match;
}

constexpr const char* board_memory_status_name(
    BoardMemoryStatus status) noexcept {
  switch (status) {
    case BoardMemoryStatus::Match:
      return "MATCH";
    case BoardMemoryStatus::FlashSizeMismatch:
      return "FLASH_SIZE_MISMATCH";
    case BoardMemoryStatus::PsramMissing:
      return "PSRAM_MISSING";
    case BoardMemoryStatus::PsramSizeMismatch:
      return "PSRAM_SIZE_MISMATCH";
  }
  return "UNKNOWN";
}

#ifdef ARDUINO
BoardMemoryReading read_runtime_board_memory() noexcept;
void log_runtime_board_memory() noexcept;
#endif

}  // namespace sigap::board
