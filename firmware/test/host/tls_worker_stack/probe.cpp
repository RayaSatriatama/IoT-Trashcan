#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>
#include "Arduino.h"
#include "HTTPClient.h"
#include "WiFi.h"
#include "network/https_transport.h"
#include "network/device_control_http.h"
#include "debug/http_transport.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
WiFiClass WiFi;
namespace {
struct Observation { std::string_view name; uint32_t depth; };
std::vector<Observation> observations;
FakeSemaphore semaphores[8]; unsigned semaphore_count = 0;
}
SemaphoreHandle_t xSemaphoreCreateMutex() { return &semaphores[semaphore_count++]; }
SemaphoreHandle_t xSemaphoreCreateBinary() { return &semaphores[semaphore_count++]; }
int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) { if (sem->held) return 0; sem->held=true; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t sem) { sem->held=false; return pdTRUE; }
int xTaskCreate(TaskFunction_t, const char* name, uint32_t depth, void*, uint32_t, void*) {
  observations.push_back({name, depth});
  return 0;
}
void vTaskDelete(void*) {}
void taskYIELD() {}
int main() {
  using namespace sigap::network;
  WiFi.connected = true;
  WiFiArchiveHttpTransport archive;
  sigap::provisioning::NetworkSecrets secrets{};
  secrets.ssid[0]='x';
  if (!archive.set_endpoint("https://example.invalid/archive") || !archive.begin(secrets,"ca")) return 2;
  NetworkHttpResponse archive_response{};
  (void)archive.start_batch("{}", "token", 1, 1000, archive_response);
  WiFiDeviceControlHttpTransport control;
  class Clock final : public IMonotonicClock { public: uint64_t now_us() const noexcept override { return 1; } } clock;
  control.bind_clock(clock);
  if (!control.set_endpoint("https://example.invalid/api/device-control/device") || !control.begin("ca")) return 3;
  DeviceControlHttpResponse control_response{};
  (void)control.start_next_command("device", "token", 1000, control_response);
  sigap::debug::DebugHttpsTransport debug("https://example.invalid/debug", "ca");
  (void)debug.start("{}", "token", 1000);
  const char* expected[] = {"sigap_net", "sigap_ctl", "sigap_dbg_http"};
  if (observations.size()!=3) return 4;
  bool valid = true;
  for (std::size_t i=0;i<observations.size();++i) {
    std::printf("observed task=%.*s stack_bytes=%u\n", static_cast<int>(observations[i].name.size()), observations[i].name.data(), observations[i].depth);
    valid = valid && observations[i].name == expected[i] && observations[i].depth == 16384;
  }
  return valid ? 0 : 1;
}
