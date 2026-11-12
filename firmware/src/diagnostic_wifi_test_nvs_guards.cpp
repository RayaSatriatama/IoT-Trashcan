#include <atomic>
#include <cstddef>
#include <cstdint>

#include <esp_err.h>
#include <esp_partition.h>
#include <nvs_flash.h>

#include "diagnostic_wifi_test_erase_trace.h"

#ifdef SIGAP_ERASE_TRACE_HOST_TEST
extern "C" void sigap_erase_trace_host_publication_pause() noexcept;
#endif

namespace {
std::atomic_bool g_erase_attempted{false};
std::atomic<SigapErasePhase> g_phase{SigapErasePhase::framework_startup};
std::atomic<uint8_t> g_record_state{0};
SigapEraseDenial g_first_denial{false, SigapErasePrimitive::none,
                                SigapErasePhase::framework_startup, 0};

void latch_erase(SigapErasePrimitive primitive, uintptr_t caller_pc) noexcept {
  const SigapErasePhase phase = g_phase.load(std::memory_order_relaxed);
  g_erase_attempted.store(true, std::memory_order_release);
  uint8_t empty = 0;
  if (!g_record_state.compare_exchange_strong(empty, 1, std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
    return;
  }
#ifdef SIGAP_ERASE_TRACE_HOST_TEST
  sigap_erase_trace_host_publication_pause();
#endif
  g_first_denial = {true, primitive, phase, caller_pc};
  g_record_state.store(2, std::memory_order_release);
}

#define SIGAP_DENY(primitive) \
  latch_erase(SigapErasePrimitive::primitive, \
              reinterpret_cast<uintptr_t>(__builtin_return_address(0)))
}

extern "C" bool sigap_ble_wifi_erase_attempted() noexcept {
  return g_erase_attempted.load(std::memory_order_acquire);
}

extern "C" SigapEraseDenial sigap_ble_wifi_first_denial() noexcept {
  if (g_record_state.load(std::memory_order_acquire) != 2) return {};
  return g_first_denial;
}

extern "C" void sigap_ble_wifi_set_phase(SigapErasePhase phase) noexcept {
  g_phase.store(phase, std::memory_order_relaxed);
}

extern "C" esp_err_t __wrap_nvs_flash_erase(void) {
  SIGAP_DENY(nvs_flash_erase);
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_flash_erase_partition(const char*) {
  SIGAP_DENY(nvs_flash_erase_partition);
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_flash_erase_partition_ptr(
    const esp_partition_t*) {
  SIGAP_DENY(nvs_flash_erase_partition_ptr);
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_erase_key(nvs_handle_t, const char*) {
  SIGAP_DENY(nvs_erase_key);
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_erase_all(nvs_handle_t) {
  SIGAP_DENY(nvs_erase_all);
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_esp_partition_erase_range(
    const esp_partition_t*, std::size_t, std::size_t) {
  SIGAP_DENY(esp_partition_erase_range);
  return ESP_ERR_INVALID_STATE;
}
