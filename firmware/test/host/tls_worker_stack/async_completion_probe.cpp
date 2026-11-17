// Covering source oracle; host/native execution remains deferred by owner.
// Uses the real production poll_response() with controlled RTOS primitives.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include "Arduino.h"
#include "HTTPClient.h"
#include "WiFi.h"
#include "network/network.h"
#include "provisioning/secret_store.h"
#define private public
#include "network/https_transport.h"
#undef private
#include "freertos/semphr.h"
#include "freertos/task.h"

WiFiClass WiFi;
SemaphoreHandle_t xSemaphoreCreateMutex() { std::abort(); }
SemaphoreHandle_t xSemaphoreCreateBinary() { std::abort(); }
int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
  if (sem->held) return 0;
  sem->held = true;
  return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t sem) { sem->held = false; return pdTRUE; }
int xTaskCreate(TaskFunction_t, const char*, uint32_t, void*, uint32_t, void*) { std::abort(); }
void vTaskDelete(void*) { std::abort(); }
void taskYIELD() { std::abort(); }

// Compile this TU with sdk_stubs.cpp and the existing fixture include paths.
// Including the producer keeps this oracle on the real implementation.
struct ForbiddenSerial {
  template <typename... Args>
  int printf(const char*, Args...) { std::abort(); }
};
static ForbiddenSerial Serial;
#include "network/https_transport.cpp"

int main() {
  using Transport = sigap::network::WiFiArchiveHttpTransport;
  using Result = sigap::network::TransportResult;
  using State = Transport::TaskState;
  unsigned passed{0U};
  auto check = [&](bool condition, const char* name) {
    if (!condition) { std::printf("FAIL %s\n", name); std::exit(1); }
    ++passed;
  };
  sigap::network::NetworkHttpResponse response{};
  Transport unstarted;
  check(unstarted.poll_response(response) == Result::Failed, "unstarted still fails");

  FakeSemaphore mutex{};
  FakeSemaphore completion{};
  Transport transport;
  transport.state_mutex_ = &mutex;
  transport.completion_ = &completion;
  transport.task_state_ = State::Running;
  completion.held = true;  // Binary semaphore has no completion token.
  check(transport.poll_response(response) == Result::Pending, "running remains pending");

  // Worker has published Complete but has not given completion_ yet.
  transport.task_state_ = State::Complete;
  check(transport.poll_response(response) == Result::Pending, "completion publication gap");
  check(transport.task_state_ == State::Complete, "gap preserves completed response");

  // Worker owns state_mutex_ while publishing, completion token still absent.
  mutex.held = true;
  check(transport.poll_response(response) == Result::Pending, "publisher mutex contention");
  check(completion.held, "no unpublished token manufactured");

  // Completion exists but state mutex is temporarily contended.
  completion.held = false;
  check(transport.poll_response(response) == Result::Pending, "consumer mutex contention");
  check(!completion.held, "contended consumer restores exact token");

  mutex.held = false;
  transport.task_result_ = Result::Response;
  transport.task_response_.status_code = 200;
  transport.task_response_.ack_present = false; // No ACK fabricated by this test.
  check(transport.poll_response(response) == Result::Response, "published response consumed");
  check(response.status_code == 200 && transport.task_state_ == State::Idle && completion.held,
        "response consumed once and transport released");
  check(transport.poll_response(response) == Result::Failed, "idle without token still fails");

  transport.task_state_ = State::Complete;
  transport.task_result_ = Result::Failed;
  completion.held = false;
  check(transport.poll_response(response) == Result::Failed && transport.task_state_ == State::Idle,
        "real completed failure remains failure");
  std::printf("PASS %u assertions; RTOS fixture, not device or network evidence\n", passed);
  return 0;
}
