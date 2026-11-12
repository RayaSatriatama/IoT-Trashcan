#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <condition_variable>
#include <mutex>

#include "diagnostic_wifi_test_erase_trace.h"
#include <esp_err.h>
#include <esp_partition.h>
#include <nvs_flash.h>

extern "C" esp_err_t __wrap_nvs_flash_erase(void);
extern "C" esp_err_t __wrap_nvs_flash_erase_partition(const char*);
extern "C" esp_err_t __wrap_nvs_flash_erase_partition_ptr(const esp_partition_t*);
extern "C" esp_err_t __wrap_nvs_erase_key(nvs_handle_t, const char*);
extern "C" esp_err_t __wrap_nvs_erase_all(nvs_handle_t);
extern "C" esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t*, std::size_t, std::size_t);

static std::atomic<unsigned> real_erase_calls{0};
static std::mutex publication_mutex;
static std::condition_variable publication_cv;
static bool publisher_paused = false;
static bool release_publisher = false;
static std::atomic<bool> pause_enabled{false};

extern "C" void sigap_erase_trace_host_publication_pause() noexcept {
  if (!pause_enabled.load(std::memory_order_relaxed)) return;
  std::unique_lock<std::mutex> lock(publication_mutex);
  publisher_paused = true;
  publication_cv.notify_all();
  publication_cv.wait(lock, [] { return release_publisher; });
}

extern "C" esp_err_t nvs_flash_erase(void) { ++real_erase_calls; return 0; }
extern "C" esp_err_t nvs_flash_erase_partition(const char*) { ++real_erase_calls; return 0; }
extern "C" esp_err_t nvs_flash_erase_partition_ptr(const esp_partition_t*) { ++real_erase_calls; return 0; }
extern "C" esp_err_t nvs_erase_key(nvs_handle_t, const char*) { ++real_erase_calls; return 0; }
extern "C" esp_err_t nvs_erase_all(nvs_handle_t) { ++real_erase_calls; return 0; }
extern "C" esp_err_t esp_partition_erase_range(const esp_partition_t*, std::size_t, std::size_t) { ++real_erase_calls; return 0; }

int main(int argc, char** argv) {
  (void)argv;
  assert(!sigap_ble_wifi_erase_attempted());
  if (argc == 2 && argv[1][0] == 'p') {
    pause_enabled.store(true, std::memory_order_relaxed);
    std::thread winner([] { assert(__wrap_nvs_flash_erase() == ESP_ERR_INVALID_STATE); });
    {
      std::unique_lock<std::mutex> lock(publication_mutex);
      publication_cv.wait(lock, [] { return publisher_paused; });
    }
    assert(!sigap_ble_wifi_first_denial().ready);
    assert(sigap_ble_wifi_erase_attempted());
    assert(__wrap_nvs_erase_all(0) == ESP_ERR_INVALID_STATE);
    assert(sigap_ble_wifi_erase_attempted());
    assert(!sigap_ble_wifi_first_denial().ready);
    sigap_ble_wifi_set_phase(SigapErasePhase::loop);
    {
      std::lock_guard<std::mutex> lock(publication_mutex);
      release_publisher = true;
    }
    publication_cv.notify_all();
    winner.join();
    const auto first = sigap_ble_wifi_first_denial();
    assert(first.ready && first.primitive == SigapErasePrimitive::nvs_flash_erase);
    assert(first.phase == SigapErasePhase::framework_startup && first.caller_pc != 0);
    return 0;
  }
  if (argc == 2 && argv[1][0] == 'r') {
    nvs_flash_erase();
    nvs_flash_erase_partition(nullptr);
    nvs_flash_erase_partition_ptr(nullptr);
    nvs_erase_key(0, nullptr);
    nvs_erase_all(0);
    esp_partition_erase_range(nullptr, 0, 0);
    assert(real_erase_calls.load() == 6);
    return 0;
  }
  if (argc == 2 && argv[1][0] == 'c') {
    std::atomic<bool> reading{true};
    std::thread reader([&] {
      while (reading.load(std::memory_order_relaxed)) {
        const auto snapshot = sigap_ble_wifi_first_denial();
        if (snapshot.ready) {
          assert(snapshot.primitive != SigapErasePrimitive::none);
          assert(snapshot.phase == SigapErasePhase::framework_startup);
          assert(snapshot.caller_pc != 0);
        }
      }
    });
    std::thread threads[] = {
      std::thread([] { __wrap_nvs_flash_erase(); }),
      std::thread([] { __wrap_nvs_flash_erase_partition(nullptr); }),
      std::thread([] { __wrap_nvs_flash_erase_partition_ptr(nullptr); }),
      std::thread([] { __wrap_nvs_erase_key(0, nullptr); }),
      std::thread([] { __wrap_nvs_erase_all(0); }),
      std::thread([] { __wrap_esp_partition_erase_range(nullptr, 0, 0); }),
    };
    for (auto& thread : threads) thread.join();
    reading.store(false, std::memory_order_relaxed);
    reader.join();
    const auto concurrent = sigap_ble_wifi_first_denial();
    assert(concurrent.ready);
    assert(concurrent.primitive != SigapErasePrimitive::none);
    assert(concurrent.phase == SigapErasePhase::framework_startup);
    assert(concurrent.caller_pc != 0);
    assert(real_erase_calls.load() == 0);
    return 0;
  }
  if (argc == 2) {
    const SigapErasePhase phase = argv[1][0] == 'b'
        ? SigapErasePhase::ble_begin
        : argv[1][0] == 'l' ? SigapErasePhase::loop
                            : SigapErasePhase::manager_begin;
    sigap_ble_wifi_set_phase(phase);
    assert(__wrap_nvs_erase_all(0) == ESP_ERR_INVALID_STATE);
    const auto staged = sigap_ble_wifi_first_denial();
    assert(staged.ready);
    assert(staged.primitive == SigapErasePrimitive::nvs_erase_all);
    assert(staged.phase == phase);
    assert(staged.caller_pc != 0);
    assert(real_erase_calls.load() == 0);
    return 0;
  }
  assert(__wrap_nvs_flash_erase() == ESP_ERR_INVALID_STATE);
  assert(sigap_ble_wifi_erase_attempted());
  const auto first = sigap_ble_wifi_first_denial();
  assert(first.ready);
  assert(first.primitive == SigapErasePrimitive::nvs_flash_erase);
  assert(first.phase == SigapErasePhase::framework_startup);
  assert(first.caller_pc != 0);

  sigap_ble_wifi_set_phase(SigapErasePhase::manager_begin);
  assert(__wrap_nvs_flash_erase_partition("ignored") == ESP_ERR_INVALID_STATE);
  assert(__wrap_nvs_flash_erase_partition_ptr(nullptr) == ESP_ERR_INVALID_STATE);
  assert(__wrap_nvs_erase_key(0, "ignored") == ESP_ERR_INVALID_STATE);
  assert(__wrap_nvs_erase_all(0) == ESP_ERR_INVALID_STATE);
  assert(__wrap_esp_partition_erase_range(nullptr, 0, 0) == ESP_ERR_INVALID_STATE);
  const auto after = sigap_ble_wifi_first_denial();
  assert(after.primitive == first.primitive);
  assert(after.phase == first.phase);
  assert(after.caller_pc == first.caller_pc);
  assert(real_erase_calls.load() == 0);
  return 0;
}
