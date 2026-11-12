#pragma once

#include <cstdint>

enum class SigapErasePrimitive : uint8_t {
  none = 0,
  nvs_flash_erase,
  nvs_flash_erase_partition,
  nvs_flash_erase_partition_ptr,
  nvs_erase_key,
  nvs_erase_all,
  esp_partition_erase_range,
};

enum class SigapErasePhase : uint8_t {
  framework_startup = 0,
  manager_begin,
  ble_begin,
  loop,
};

struct SigapEraseDenial {
  bool ready;
  SigapErasePrimitive primitive;
  SigapErasePhase phase;
  uintptr_t caller_pc;
};

extern "C" bool sigap_ble_wifi_erase_attempted() noexcept;
extern "C" SigapEraseDenial sigap_ble_wifi_first_denial() noexcept;
extern "C" void sigap_ble_wifi_set_phase(SigapErasePhase phase) noexcept;
