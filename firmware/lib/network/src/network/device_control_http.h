#pragma once

#include "network/device_control.h"
#include "network/device_control_codec.h"
#include "network/https_transport.h"

#ifdef ARDUINO

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sigap::network {

inline constexpr std::size_t kMaxDeviceControlEndpointBytes{192U};

// A separate bounded task owns control-plane HTTP so archive delivery and
// acquisition polling never execute a synchronous request in their call path.
// The endpoint is the HTTPS `/api/device-control/device` prefix.
class WiFiDeviceControlHttpTransport final : public IDeviceControlTransport {
 public:
  WiFiDeviceControlHttpTransport() noexcept = default;

  bool set_endpoint(std::string_view endpoint) noexcept;
  bool begin(const char* ca_certificate) noexcept;
  void bind_clock(const IMonotonicClock& clock) noexcept { clock_ = &clock; }
  bool connected() const noexcept;

  TransportResult start_next_command(
      std::string_view device_id, std::string_view token, uint32_t timeout_ms,
      DeviceControlHttpResponse& response) noexcept override;
  TransportResult poll_next_command(
      DeviceControlHttpResponse& response) noexcept override;
  TransportResult start_command_result(
      std::string_view device_id, std::string_view token,
      const DeviceControlCommand& command, const DeviceControlResult& result,
      uint32_t timeout_ms, DeviceControlHttpResponse& response) noexcept override;
  TransportResult poll_command_result(
      DeviceControlHttpResponse& response) noexcept override;
  void cancel_command_request() noexcept override;

 private:
  enum class RequestKind : uint8_t { Next, Result };
  enum class TaskState : uint8_t { Idle, Running, Complete };

  bool start(RequestKind kind, std::string_view device_id,
             std::string_view token, std::string_view command_id,
             const DeviceControlWireBody* body, uint32_t timeout_ms,
             DeviceControlHttpResponse& response) noexcept;
  TransportResult poll(DeviceControlHttpResponse& response) noexcept;
  bool build_url(std::string_view device_id, std::string_view command_id) noexcept;
  static void task(void* context) noexcept;
  TransportResult run_request() noexcept;

  std::array<char, kMaxDeviceControlEndpointBytes + 1U> endpoint_{};
  std::array<char, kMaxDeviceControlEndpointBytes + kMaxDeviceControlIdBytes * 2U + 16U>
      request_url_{};
  DeviceControlWireBody request_body_{};
  std::array<char, kMaxDeviceControlWireBytes> response_body_{};
  std::size_t response_body_size_{0U};
  const char* ca_certificate_{nullptr};
  const IMonotonicClock* clock_{nullptr};
  const char* token_{nullptr};
  std::size_t token_size_{0U};
  uint32_t timeout_ms_{0U};
  RequestKind request_kind_{RequestKind::Next};
  TaskState task_state_{TaskState::Idle};
  bool configured_{false};
  bool cancelled_{false};
  TransportResult task_result_{TransportResult::Failed};
  DeviceControlHttpResponse task_response_{};
  SemaphoreHandle_t mutex_{nullptr};
  SemaphoreHandle_t completion_{nullptr};
};

}  // namespace sigap::network

#endif  // ARDUINO
