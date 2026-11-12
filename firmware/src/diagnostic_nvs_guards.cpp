#include <atomic>
#include <cstddef>
#include <cstring>

#include <esp_err.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace {
std::atomic_bool g_failed{false};

void latch_failure() noexcept { g_failed.store(true, std::memory_order_relaxed); }

}  // namespace

extern "C" bool sigap_ble_diag_failed() noexcept {
  return g_failed.load(std::memory_order_relaxed);
}


extern "C" esp_err_t __real_nvs_flash_init(void);
extern "C" esp_err_t __wrap_nvs_flash_init(void) {
  const esp_err_t result = __real_nvs_flash_init();
  if (result != ESP_OK) latch_failure();
  return result;
}

extern "C" esp_err_t __real_nvs_flash_init_partition(const char*);
extern "C" esp_err_t __wrap_nvs_flash_init_partition(const char* label) {
  const esp_err_t result = __real_nvs_flash_init_partition(label);
  if (result != ESP_OK) latch_failure();
  return result;
}

extern "C" esp_err_t __real_nvs_flash_init_partition_ptr(const esp_partition_t*);
extern "C" esp_err_t __wrap_nvs_flash_init_partition_ptr(
    const esp_partition_t* partition) {
  const esp_err_t result = __real_nvs_flash_init_partition_ptr(partition);
  if (result != ESP_OK) latch_failure();
  return result;
}

extern "C" esp_err_t __wrap_nvs_flash_erase(void) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_flash_erase_partition(const char*) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_flash_erase_partition_ptr(const esp_partition_t*) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_esp_partition_erase_range(
    const esp_partition_t*, std::size_t, std::size_t) {
  // Diagnostic authorization excludes page erases, including NVS GC.
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_open(const char* name, nvs_open_mode_t,
                                      nvs_handle_t*) {
  // BLE radio calibration falls back when its optional NVS record is unavailable.
  if (name != nullptr && std::strcmp(name, "phy") == 0) {
    return ESP_ERR_INVALID_STATE;
  }
  // Keep application and BLE-store record access out of the diagnostic image.
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_get_blob(
    nvs_handle_t, const char*, void*, std::size_t*) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_set_blob(
    nvs_handle_t, const char*, const void*, std::size_t) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_commit(nvs_handle_t) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_erase_key(nvs_handle_t, const char*) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __wrap_nvs_erase_all(nvs_handle_t) {
  latch_failure();
  return ESP_ERR_INVALID_STATE;
}
