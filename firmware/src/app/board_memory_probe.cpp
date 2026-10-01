#include "sigap/board/board_memory.h"

#ifdef ARDUINO

#include <Arduino.h>

namespace sigap::board {

BoardMemoryReading read_runtime_board_memory() noexcept {
  return BoardMemoryReading{static_cast<uint64_t>(ESP.getFlashChipSize()),
                            psramFound(),
                            static_cast<uint64_t>(ESP.getPsramSize())};
}

void log_runtime_board_memory() noexcept {
  const BoardMemoryReading reading = read_runtime_board_memory();
  const BoardMemoryStatus status = evaluate_board_memory(reading);
  Serial.printf(
      "[SIGAP] board memory flash=%llu bytes psram_present=%s psram=%llu "
      "bytes status=%s\\n",
      static_cast<unsigned long long>(reading.flash_bytes),
      reading.psram_present ? "true" : "false",
      static_cast<unsigned long long>(reading.psram_bytes),
      board_memory_status_name(status));
  if (status != BoardMemoryStatus::Match) {
    Serial.printf(
        "[SIGAP] BOARD_MEMORY_MISMATCH expected_flash=%llu "
        "expected_psram=%llu\\n",
        static_cast<unsigned long long>(kExpectedFlashBytes),
        static_cast<unsigned long long>(kExpectedPsramBytes));
  }
}

}  // namespace sigap::board

#endif
