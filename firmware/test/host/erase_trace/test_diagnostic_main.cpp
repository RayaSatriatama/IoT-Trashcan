#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include "diagnostic_wifi_test_erase_trace.h"

namespace {
enum class Trigger { none, manager_begin, manager_poll, ble_poll };
Trigger trigger = Trigger::none;
int wifi_begin_calls = 0;
int wifi_poll_calls = 0;
int ble_begin_calls = 0;
int ble_poll_calls = 0;
int delay_calls = 0;
std::string serial_output;
std::mutex pause_mutex;
std::condition_variable pause_cv;
bool publisher_paused = false;
bool release_publisher = false;
std::atomic<bool> pause_enabled{false};
}

FakeSerial Serial;
FakeEfuse ESP;
FakeWiFi WiFi;
void FakeSerial::begin(unsigned) noexcept {}
void FakeSerial::print(const char* text) noexcept { serial_output += text; }
void FakeSerial::println(const char* text) noexcept { serial_output += text; serial_output += '\n'; }
#include <cstdarg>
#include <cstdio>
void FakeSerial::printf(const char* format, ...) noexcept {
  char line[160]{};
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  serial_output += line;
}
void delay(unsigned) noexcept { ++delay_calls; }

extern "C" void sigap_erase_trace_host_publication_pause() noexcept {
  if (!pause_enabled.load(std::memory_order_relaxed)) return;
  std::unique_lock<std::mutex> lock(pause_mutex);
  publisher_paused = true;
  pause_cv.notify_all();
  pause_cv.wait(lock, [] { return release_publisher; });
}

extern "C" esp_err_t __wrap_nvs_erase_all(nvs_handle_t);
extern "C" esp_err_t __wrap_nvs_flash_erase(void);

#include "diagnostic_wifi_test_main.cpp"

namespace sigap::provisioning {
bool WifiConnectionManager::begin() noexcept {
  ++wifi_begin_calls;
  if (trigger == Trigger::manager_begin) __wrap_nvs_erase_all(0);
  return true;
}
void WifiConnectionManager::poll() noexcept {
  ++wifi_poll_calls;
  if (trigger == Trigger::manager_poll && wifi_poll_calls == 1) __wrap_nvs_erase_all(0);
}
bool ArduinoBleProvisioningService::begin(BleProvisioningSession&, std::string_view) noexcept {
  ++ble_begin_calls;
  return true;
}
void ArduinoBleProvisioningService::poll() noexcept {
  ++ble_poll_calls;
  if (trigger == Trigger::ble_poll && ble_poll_calls == 1) __wrap_nvs_erase_all(0);
}
}

extern "C" esp_err_t __wrap_nvs_flash_erase(void);

static void assert_trace_precedes_stop() {
  const auto trace = serial_output.find("BLE_WIFI_TEST_ERASE");
  const auto stop = serial_output.find("BLE_WIFI_TEST_STOP");
  assert(trace != std::string::npos && stop != std::string::npos && trace < stop);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  if (mode == "startup") {
    __wrap_nvs_erase_all(0);
    setup();
    assert(halted && wifi_begin_calls == 0 && ble_begin_calls == 0 && ble_poll_calls == 0);
    assert_trace_precedes_stop();
    return 0;
  }
  if (mode == "manager-begin") trigger = Trigger::manager_begin;
  setup();
  if (mode == "manager-begin") {
    assert(halted && ble_begin_calls == 0 && ble_poll_calls == 0);
    assert_trace_precedes_stop();
    return 0;
  }
  assert(!halted && ble_begin_calls == 1);
  if (mode == "entry") __wrap_nvs_erase_all(0);
  if (mode == "manager-poll") trigger = Trigger::manager_poll;
  if (mode == "ble-poll") trigger = Trigger::ble_poll;
  if (mode == "paused") {
    pause_enabled.store(true, std::memory_order_relaxed);
    std::thread publisher([] { __wrap_nvs_flash_erase(); });
    {
      std::unique_lock<std::mutex> lock(pause_mutex);
      pause_cv.wait(lock, [] { return publisher_paused; });
    }
    loop();
    assert(halted && ble_poll_calls == 1 && wifi_poll_calls == 0);
    assert(serial_output.find("BLE_WIFI_TEST_STOP") == std::string::npos);
    {
      std::lock_guard<std::mutex> lock(pause_mutex);
      release_publisher = true;
    }
    pause_cv.notify_all();
    publisher.join();
    loop();
    assert_trace_precedes_stop();
  } else {
    loop();
    assert(halted);
    if (mode == "entry") assert(wifi_poll_calls == 0 && ble_poll_calls == 1);
    if (mode == "manager-poll") assert(wifi_poll_calls == 1 && ble_poll_calls == 1);
    if (mode == "ble-poll") assert(wifi_poll_calls == 1 && ble_poll_calls == 2);
    for (int i = 0; i < 100; ++i) loop();
    assert(wifi_poll_calls == (mode == "entry" ? 0 : 1));
    assert(ble_poll_calls == (mode == "ble-poll" ? 2 : 1));
    assert_trace_precedes_stop();
  }
  assert(ble_failure_cleanup_done);
  assert(ble_poll_calls == (mode == "ble-poll" ? 2 : 1));
  assert(wifi_poll_calls == ((mode == "entry" || mode == "paused") ? 0 : 1));
  return 0;
}
