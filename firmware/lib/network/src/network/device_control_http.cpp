#include "network/device_control_http.h"

#ifdef ARDUINO

#include <algorithm>

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/task.h>

namespace sigap::network {
namespace {

// WiFiClientSecure and HTTPClient use more than 4 KiB of stack during TLS
// setup on ESP32-S3.  Keep the control request isolated from loopTask while
// leaving enough bounded headroom for certificate parsing and JSON framing.
constexpr uint32_t kControlTaskStackWords{4096U};

bool https_prefix(std::string_view value) noexcept {
  constexpr std::string_view prefix{"https://"};
  return value.size() > prefix.size() &&
         value.substr(0U, prefix.size()) == prefix &&
         value.find_first_of("?#") == std::string_view::npos;
}

bool path_component(std::string_view value) noexcept {
  if (value.empty()) return false;
  for (const char byte : value) {
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
          byte == '.')) {
      return false;
    }
  }
  return true;
}

bool append(char* output, std::size_t capacity, std::size_t& size,
            std::string_view value) noexcept {
  if (size + value.size() >= capacity) return false;
  std::copy(value.begin(), value.end(), output + size);
  size += value.size();
  output[size] = '\0';
  return true;
}

}  // namespace

bool WiFiDeviceControlHttpTransport::set_endpoint(
    std::string_view endpoint) noexcept {
  if (!https_prefix(endpoint) || endpoint.size() >= endpoint_.size()) {
    endpoint_[0] = '\0';
    return false;
  }
  std::copy(endpoint.begin(), endpoint.end(), endpoint_.begin());
  endpoint_[endpoint.size()] = '\0';
  return true;
}

bool WiFiDeviceControlHttpTransport::begin(const char* ca_certificate) noexcept {
  configured_ = endpoint_[0] != '\0' && ca_certificate != nullptr &&
                ca_certificate[0] != '\0';
  ca_certificate_ = configured_ ? ca_certificate : nullptr;
  return configured_;
}

bool WiFiDeviceControlHttpTransport::connected() const noexcept {
  return configured_ && WiFi.status() == WL_CONNECTED;
}

bool WiFiDeviceControlHttpTransport::build_url(
    std::string_view device_id, std::string_view command_id) noexcept {
  if (!path_component(device_id) ||
      (!command_id.empty() && !path_component(command_id))) {
    return false;
  }
  std::size_t size{0U};
  if (!append(request_url_.data(), request_url_.size(), size,
              std::string_view(endpoint_.data())) ||
      !append(request_url_.data(), request_url_.size(), size, "/") ||
      !append(request_url_.data(), request_url_.size(), size, device_id) ||
      (command_id.empty()
           ? !append(request_url_.data(), request_url_.size(), size, "/next")
           : (!append(request_url_.data(), request_url_.size(), size, "/") ||
              !append(request_url_.data(), request_url_.size(), size, command_id) ||
              !append(request_url_.data(), request_url_.size(), size, "/result")))) {
    return false;
  }
  return true;
}

bool WiFiDeviceControlHttpTransport::start(
    RequestKind kind, std::string_view device_id, std::string_view token,
    std::string_view command_id, const DeviceControlWireBody* body,
    uint32_t timeout_ms, DeviceControlHttpResponse& response) noexcept {
  response = DeviceControlHttpResponse{};
  if (!configured_ || clock_ == nullptr || !connected() || token.empty() || token.size() > 128U ||
      !build_url(device_id, command_id) ||
      (kind == RequestKind::Result && (body == nullptr || body->size == 0U))) {
    return false;
  }
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  if (completion_ == nullptr) completion_ = xSemaphoreCreateBinary();
  if (mutex_ == nullptr || completion_ == nullptr ||
      xSemaphoreTake(mutex_, 0U) != pdTRUE) {
    return false;
  }
  if (task_state_ != TaskState::Idle) {
    xSemaphoreGive(mutex_);
    return false;
  }
  (void)xSemaphoreTake(completion_, 0U);
  request_kind_ = kind;
  request_body_ = body == nullptr ? DeviceControlWireBody{} : *body;
  token_ = token.data();
  token_size_ = token.size();
  timeout_ms_ = timeout_ms == 0U ? kHttpTimeoutMs : timeout_ms;
  task_response_ = DeviceControlHttpResponse{};
  task_result_ = TransportResult::Failed;
  cancelled_ = false;
  task_state_ = TaskState::Running;
  xSemaphoreGive(mutex_);
  if (xTaskCreate(&WiFiDeviceControlHttpTransport::task, "sigap_ctl",
                  kControlTaskStackWords, this, 1U, nullptr) != pdPASS) {
    if (xSemaphoreTake(mutex_, 0U) == pdTRUE) {
      task_state_ = TaskState::Idle;
      xSemaphoreGive(mutex_);
    }
    return false;
  }
  return true;
}

TransportResult WiFiDeviceControlHttpTransport::start_next_command(
    std::string_view device_id, std::string_view token, uint32_t timeout_ms,
    DeviceControlHttpResponse& response) noexcept {
  return start(RequestKind::Next, device_id, token, {}, nullptr, timeout_ms,
               response)
             ? TransportResult::Pending
             : TransportResult::Failed;
}

TransportResult WiFiDeviceControlHttpTransport::start_command_result(
    std::string_view device_id, std::string_view token,
    const DeviceControlCommand& command, const DeviceControlResult& result,
    uint32_t timeout_ms, DeviceControlHttpResponse& response) noexcept {
  DeviceControlWireBody wire{};
  if (!encode_device_control_result(result, wire)) return TransportResult::Failed;
  return start(RequestKind::Result, device_id, token, command.command_id.view(),
               &wire, timeout_ms, response)
             ? TransportResult::Pending
             : TransportResult::Failed;
}

TransportResult WiFiDeviceControlHttpTransport::poll(
    DeviceControlHttpResponse& response) noexcept {
  if (completion_ == nullptr || xSemaphoreTake(completion_, 0U) != pdTRUE) {
    if (mutex_ != nullptr && xSemaphoreTake(mutex_, 0U) == pdTRUE) {
      const auto state = task_state_;
      xSemaphoreGive(mutex_);
      return state == TaskState::Running ? TransportResult::Pending
                                         : TransportResult::Failed;
    }
    return TransportResult::Failed;
  }
  if (mutex_ == nullptr || xSemaphoreTake(mutex_, 0U) != pdTRUE) {
    (void)xSemaphoreGive(completion_);
    return TransportResult::Failed;
  }
  if (task_state_ != TaskState::Complete) {
    xSemaphoreGive(mutex_);
    return TransportResult::Failed;
  }
  response = task_response_;
  const auto result = task_result_;
  task_state_ = TaskState::Idle;
  xSemaphoreGive(mutex_);
  return result;
}

TransportResult WiFiDeviceControlHttpTransport::poll_next_command(
    DeviceControlHttpResponse& response) noexcept {
  return poll(response);
}

TransportResult WiFiDeviceControlHttpTransport::poll_command_result(
    DeviceControlHttpResponse& response) noexcept {
  return poll(response);
}

void WiFiDeviceControlHttpTransport::cancel_command_request() noexcept {
  if (mutex_ != nullptr && xSemaphoreTake(mutex_, 0U) == pdTRUE) {
    if (task_state_ == TaskState::Running) cancelled_ = true;
    xSemaphoreGive(mutex_);
  }
}

void WiFiDeviceControlHttpTransport::task(void* context) noexcept {
  auto& self = *static_cast<WiFiDeviceControlHttpTransport*>(context);
  const auto result = self.run_request();
  if (self.mutex_ != nullptr && xSemaphoreTake(self.mutex_, portMAX_DELAY) == pdTRUE) {
    self.task_result_ = self.cancelled_ ? TransportResult::Failed : result;
    self.task_state_ = TaskState::Complete;
    xSemaphoreGive(self.mutex_);
  }
  if (self.completion_ != nullptr) (void)xSemaphoreGive(self.completion_);
  vTaskDelete(nullptr);
}

TransportResult WiFiDeviceControlHttpTransport::run_request() noexcept {
  if (!configured_ || !connected() || ca_certificate_ == nullptr ||
      token_ == nullptr || token_size_ == 0U) {
    return TransportResult::Failed;
  }
  std::array<char, kMaxAuthorizationBytes> authorization{};
  constexpr std::string_view prefix{"Bearer "};
  if (prefix.size() + token_size_ >= authorization.size()) return TransportResult::Failed;
  std::copy(prefix.begin(), prefix.end(), authorization.begin());
  std::copy_n(token_, token_size_, authorization.begin() + prefix.size());

  WiFiClientSecure client;
  client.setCACert(ca_certificate_);
  client.setTimeout(timeout_ms_);
  HTTPClient http;
  // HTTPClient::begin passes the URL hostname to WiFiClientSecure. With a CA
  // configured and no insecure mode, both certificate-chain and hostname
  // verification remain mandatory.
  if (!http.begin(client, request_url_.data())) return TransportResult::Failed;
  http.setTimeout(timeout_ms_);
  http.addHeader("Authorization", authorization.data());
  http.addHeader("Accept", "application/json");
  int status{0};
  if (request_kind_ == RequestKind::Next) {
    status = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    status = http.POST(reinterpret_cast<uint8_t*>(request_body_.bytes.data()),
                       request_body_.size);
  }
  task_response_ = DeviceControlHttpResponse{};
  task_response_.status_code = status;
  if (status <= 0) {
    http.end();
    return TransportResult::Timeout;
  }
  response_body_size_ = 0U;
  Stream* stream = http.getStreamPtr();
  const uint32_t started = millis();
  const int length = http.getSize();
  const std::size_t expected = length < 0 ? 0U : static_cast<std::size_t>(length);
  if (expected > response_body_.size()) {
    http.end();
    return TransportResult::Failed;
  }
  while (stream != nullptr && millis() - started < timeout_ms_) {
    if (length >= 0 && response_body_size_ >= expected) break;
    const std::size_t available = static_cast<std::size_t>(stream->available());
    if (available == 0U) {
      vTaskDelay(1U);
      continue;
    }
    const std::size_t amount = std::min(available, response_body_.size() - response_body_size_);
    if (amount == 0U) {
      http.end();
      return TransportResult::Failed;
    }
    const int count = stream->readBytes(response_body_.data() + response_body_size_, amount);
    if (count <= 0) {
      vTaskDelay(1U);
      continue;
    }
    response_body_size_ += static_cast<std::size_t>(count);
  }
  const bool incomplete = (length >= 0 && response_body_size_ != expected) ||
                          (length < 0 && stream != nullptr && stream->available() > 0);
  http.end();
  if (incomplete) return TransportResult::Timeout;
  if (request_kind_ == RequestKind::Next && status == 200) {
    task_response_.command_present = clock_ != nullptr &&
        decode_device_control_command(
            std::string_view(response_body_.data(), response_body_size_),
            clock_->now_us(), task_response_.command);
    if (!task_response_.command_present) return TransportResult::Failed;
  }
  return TransportResult::Response;
}

}  // namespace sigap::network

#endif  // ARDUINO
