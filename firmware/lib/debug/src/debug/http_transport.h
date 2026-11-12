#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "debug/http_worker.h"
#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif
namespace sigap::debug {
#ifdef ARDUINO
// Endpoint and CA are composition-owned, stable for the transport lifetime.
class DebugHttpsTransport final : public IDebugHttpTransport {
 public:
  DebugHttpsTransport(std::string_view endpoint, const char* ca_certificate) noexcept;
  ~DebugHttpsTransport() override = default;
  DebugHttpsTransport(const DebugHttpsTransport&) = delete;
  DebugHttpsTransport& operator=(const DebugHttpsTransport&) = delete;
  network::TransportResult start(std::string_view exact_body, std::string_view token,
                                 uint32_t timeout_ms) noexcept override;
  network::TransportResult poll(DebugHttpResult& result) noexcept override;
  void cancel() noexcept override;
  bool quiescent() const noexcept override;
 private:
  enum class State : uint8_t { Idle, Running, Complete };
  static void task_entry(void*) noexcept;
  void run_task() noexcept;
  void publish(const DebugHttpResult&, network::TransportResult) noexcept;
  std::array<char, 193> endpoint_{};
  const char* ca_{};
  SemaphoreHandle_t mutex_{};
  State state_{State::Idle};
  bool cancel_requested_{}, result_ready_{}, result_published_{}, is_quiescent_{true};
  network::TransportResult result_{network::TransportResult::Failed};
  DebugHttpResult response_{};
  const char* body_{};
  size_t body_size_{};
  const char* token_{};
  size_t token_size_{};
  uint32_t timeout_ms_{};
};
#endif
}
