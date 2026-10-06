#include "sigap/board/board_memory.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp32s3/spiram.h>

namespace sigap::board {

BoardMemoryReading read_runtime_board_memory() noexcept {
  const bool present = psramFound();
  return BoardMemoryReading{
      static_cast<uint64_t>(ESP.getFlashChipSize()),
      present,
      present ? static_cast<uint64_t>(esp_spiram_get_size()) : 0U,
      present ? static_cast<uint64_t>(heap_caps_get_total_size(
                   MALLOC_CAP_SPIRAM))
              : 0U,
      present ? static_cast<uint64_t>(heap_caps_get_free_size(
                   MALLOC_CAP_SPIRAM))
              : 0U,
      present ? static_cast<uint64_t>(heap_caps_get_largest_free_block(
                   MALLOC_CAP_SPIRAM))
              : 0U};
}

void log_runtime_board_memory() noexcept {
  const BoardMemoryReading reading = read_runtime_board_memory();
  const BoardMemoryStatus status = evaluate_board_memory(reading);
  Serial.printf(
      "[SIGAP] board memory flash=%llu bytes psram_present=%s "
      "psram_physical=%llu bytes psram_heap_total=%llu "
      "psram_heap_free=%llu psram_heap_largest=%llu status=%s\\n",
      static_cast<unsigned long long>(reading.flash_bytes),
      reading.psram_present ? "true" : "false",
      static_cast<unsigned long long>(reading.psram_physical_bytes),
      static_cast<unsigned long long>(reading.psram_heap_total_bytes),
      static_cast<unsigned long long>(reading.psram_heap_free_bytes),
      static_cast<unsigned long long>(
          reading.psram_heap_largest_free_block_bytes),
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
