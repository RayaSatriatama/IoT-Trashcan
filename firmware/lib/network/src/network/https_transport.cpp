#include "https_transport.h"

#include <algorithm>
#include <cctype>
#include <limits>

#ifdef ARDUINO
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

namespace sigap::network {
namespace {

#ifdef ARDUINO
// Phases: 1=request validation, 2=readiness bitmask, 3=http.begin,
// 4=POST status, 5=ACK decoded. Readiness bits: configured=1, connected=2,
// CA present=4. Keep fields numeric; never emit request/response contents.
void log_http_diagnostic(uint8_t phase, int code, uint16_t records,
                         std::size_t body_bytes, bool ack,
                         bool truncated) noexcept {
  Serial.printf("SIGAP_HTTP phase=%u code=%d records=%u body_bytes=%u ack=%u truncated=%u\n",
                static_cast<unsigned>(phase), code,
                static_cast<unsigned>(records),
                static_cast<unsigned>(body_bytes), ack ? 1U : 0U,
                truncated ? 1U : 0U);
}
#endif

std::size_t bounded_c_string(const char* value, std::size_t capacity) noexcept {
  if (value == nullptr) return capacity + 1U;
  std::size_t length{0U};
  while (length < capacity && value[length] != '\0') ++length;
  return length;
}

bool https_endpoint(std::string_view endpoint) noexcept {
  constexpr std::string_view prefix{"https://"};
  return endpoint.size() > prefix.size() &&
         endpoint.substr(0U, prefix.size()) == prefix;
}

struct JsonCursor final {
  std::string_view input{};
  std::size_t position{0U};

  void skip_space() noexcept {
    while (position < input.size() &&
           std::isspace(static_cast<unsigned char>(input[position])) != 0) {
      ++position;
    }
  }

  bool take(char expected) noexcept {
    skip_space();
    if (position >= input.size() || input[position] != expected) return false;
    ++position;
    return true;
  }

  bool string(char* output, std::size_t capacity,
              std::size_t& output_size) noexcept {
    skip_space();
    if (position >= input.size() || input[position] != '"' || capacity == 0U) {
      return false;
    }
    ++position;
    output_size = 0U;
    while (position < input.size() && input[position] != '"') {
      const auto byte = static_cast<unsigned char>(input[position]);
      // The ACK contract uses plain bounded ASCII strings. Reject escapes and
      // controls instead of attempting partial JSON unescaping.
      if (byte < 0x20U || input[position] == '\\' ||
          output_size + 1U >= capacity) {
        return false;
      }
      output[output_size++] = input[position++];
    }
    if (position >= input.size()) return false;
    ++position;
    output[output_size] = '\0';
    return true;
  }

  bool uint16(uint16_t& output) noexcept {
    skip_space();
    if (position >= input.size() || input[position] < '0' ||
        input[position] > '9') {
      return false;
    }
    uint32_t value{0U};
    const std::size_t begin = position;
    while (position < input.size() && input[position] >= '0' &&
           input[position] <= '9') {
      const uint32_t digit = static_cast<uint32_t>(input[position++] - '0');
      if (value > (std::numeric_limits<uint16_t>::max() - digit) / 10U) {
        return false;
      }
      value = value * 10U + digit;
    }
    if (position - begin > 1U && input[begin] == '0') return false;
    output = static_cast<uint16_t>(value);
    return true;
  }
};

uint8_t hex_nibble(char byte) noexcept {
  if (byte >= '0' && byte <= '9') return static_cast<uint8_t>(byte - '0');
  if (byte >= 'a' && byte <= 'f') return static_cast<uint8_t>(byte - 'a' + 10);
  if (byte >= 'A' && byte <= 'F') return static_cast<uint8_t>(byte - 'A' + 10);
  return 0xffU;
}

bool decode_hex(std::string_view value, uint8_t* output,
                std::size_t output_size) noexcept {
  if (value.size() != output_size * 2U) return false;
  for (std::size_t index = 0U; index < output_size; ++index) {
    const uint8_t high = hex_nibble(value[index * 2U]);
    const uint8_t low = hex_nibble(value[index * 2U + 1U]);
    if (high == 0xffU || low == 0xffU) return false;
    output[index] = static_cast<uint8_t>((high << 4U) | low);
  }
  return true;
}

}  // namespace

bool valid_archive_http_request(const HttpPostRequest& request) noexcept {
  return https_endpoint(request.endpoint) &&
         request.endpoint.size() <= kMaxEndpointBytes &&
         !request.bearer_token.empty() && request.bearer_token.size() <= 128U &&
         !request.exact_body.empty() &&
         request.exact_body.size() <= kMaxArchiveBodyBytes &&
         request.record_count != 0U &&
         request.record_count <= kMaxArchiveRecords;
}

uint32_t parse_retry_after_ms(std::string_view value) noexcept {
  std::size_t begin{0U};
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  if (begin == value.size()) return 0U;
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  if (end == begin) return 0U;
  uint64_t seconds{0U};
  for (std::size_t index = begin; index < end; ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte < static_cast<unsigned char>('0') ||
        byte > static_cast<unsigned char>('9')) {
      return 0U;
    }
    const uint64_t digit = static_cast<uint64_t>(byte - '0');
    if (seconds > (UINT32_MAX - digit) / 10U) return UINT32_MAX;
    seconds = seconds * 10U + digit;
    if (seconds > UINT32_MAX / 1000U) return UINT32_MAX;
  }
  return static_cast<uint32_t>(seconds * 1000U);
}

HttpPostStatus classify_http_status(int status_code) noexcept {
  if (status_code >= 200 && status_code < 300) {
    return HttpPostStatus::Delivered;
  }
  if (status_code == 401 || status_code == 403) {
    return HttpPostStatus::Unauthorized;
  }
  if (status_code == 409) return HttpPostStatus::Conflict;
  if (status_code == 408 || status_code == 425 || status_code == 429 ||
      status_code >= 500) {
    return HttpPostStatus::Retryable;
  }
  return HttpPostStatus::ProtocolError;
}

bool decode_archive_ack(std::string_view body,
                        telemetry_archive::Ack& output) noexcept {
  output = telemetry_archive::Ack{};
  if (body.empty() || body.size() > kMaxResponseBodyBytes) return false;
  char key[32]{};
  char device[telemetry_archive::kMaxBatchIdBytes]{};
  char batch[telemetry_archive::kMaxBatchIdBytes]{};
  char hash[telemetry_archive::kMaxBatchIdBytes]{};
  char status[16]{};
  std::size_t device_size{0U};
  std::size_t batch_size{0U};
  std::size_t hash_size{0U};
  std::size_t status_size{0U};
  uint8_t seen{0U};
  uint16_t record_count{0U};
  uint16_t stored_count{0U};
  uint16_t deduplicated_count{0U};
  JsonCursor cursor{body};
  if (!cursor.take('{')) return false;
  cursor.skip_space();
  if (cursor.position < body.size() && body[cursor.position] == '}') return false;
  while (cursor.position < body.size()) {
    std::size_t key_size{0U};
    if (!cursor.string(key, sizeof(key), key_size) || !cursor.take(':')) return false;
    const std::string_view field(key, key_size);
    uint8_t bit{0U};
    if (field == "device_id") bit = 1U;
    else if (field == "batch_id") bit = 2U;
    else if (field == "body_hash") bit = 4U;
    else if (field == "record_count") bit = 8U;
    else if (field == "stored_count") bit = 16U;
    else if (field == "deduplicated_count") bit = 32U;
    else if (field == "status") bit = 64U;
    else return false;
    if ((seen & bit) != 0U) return false;
    seen = static_cast<uint8_t>(seen | bit);
    switch (bit) {
      case 1U: if (!cursor.string(device, sizeof(device), device_size)) return false; break;
      case 2U: if (!cursor.string(batch, sizeof(batch), batch_size)) return false; break;
      case 4U: if (!cursor.string(hash, sizeof(hash), hash_size)) return false; break;
      case 64U: if (!cursor.string(status, sizeof(status), status_size)) return false; break;
      case 8U: if (!cursor.uint16(record_count)) return false; break;
      case 16U: if (!cursor.uint16(stored_count)) return false; break;
      case 32U: if (!cursor.uint16(deduplicated_count)) return false; break;
      default: return false;
    }
    cursor.skip_space();
    if (cursor.position >= body.size()) return false;
    if (body[cursor.position] == '}') { ++cursor.position; break; }
    if (!cursor.take(',')) return false;
  }
  cursor.skip_space();
  if (cursor.position != body.size() || seen != 127U ||
      std::string_view(status, status_size) != "DURABLE" ||
      hash_size != 71U || std::string_view(hash, 7U) != "sha256:") {
    return false;
  }
  if (!decode_hex(std::string_view(device, device_size),
                  output.device_id.bytes.data(), output.device_id.bytes.size()) ||
      batch_size == 0U || batch_size >= output.batch_id.size() ||
      !decode_hex(std::string_view(hash + 7, hash_size - 7U),
                  output.body_hash.data(), output.body_hash.size())) {
    return false;
  }
  std::copy(batch, batch + batch_size, output.batch_id.begin());
  output.batch_id_size = batch_size;
  output.device_id.present = true;
  output.record_count = record_count;
  output.stored_count = stored_count;
  output.deduplicated_count = deduplicated_count;
  if (output.record_count == 0U || output.record_count > kMaxArchiveRecords ||
      static_cast<uint32_t>(output.stored_count) + output.deduplicated_count !=
          output.record_count) {
    output = telemetry_archive::Ack{};
    return false;
  }
  output.status = telemetry_archive::AckStatus::Durable;
  return true;
}

#ifdef ARDUINO

bool WiFiArchiveHttpTransport::begin(
    const provisioning::NetworkSecrets& secrets,
    const char* ca_certificate) noexcept {
  const std::size_t ssid_size =
      bounded_c_string(secrets.ssid.data(), secrets.ssid.size());
  const std::size_t password_size =
      bounded_c_string(secrets.password.data(), secrets.password.size());
  if (ssid_size == 0U || ssid_size >= secrets.ssid.size() ||
      password_size >= secrets.password.size() || ca_certificate == nullptr ||
      ca_certificate[0] == '\0') {
    configured_ = false;
    return false;
  }
  ca_certificate_ = ca_certificate;
  WiFi.mode(WIFI_STA);
  WiFi.begin(secrets.ssid.data(), secrets.password.data());
  configured_ = true;
  return true;
}

bool WiFiArchiveHttpTransport::set_endpoint(std::string_view endpoint) noexcept {
  if (!https_endpoint(endpoint) || endpoint.size() > kMaxEndpointBytes) {
    endpoint_[0] = '\0';
    return false;
  }
  std::copy(endpoint.begin(), endpoint.end(), endpoint_.begin());
  endpoint_[endpoint.size()] = '\0';
  return true;
}

bool WiFiArchiveHttpTransport::connected() const noexcept {
  return configured_ && WiFi.status() == WL_CONNECTED;
}

TransportResult WiFiArchiveHttpTransport::start_batch(
    std::string_view exact_body, std::string_view bearer_token,
    uint16_t record_count, uint32_t timeout_ms,
    NetworkHttpResponse& response) noexcept {
  response = NetworkHttpResponse{};
  const HttpPostRequest request{
      std::string_view(endpoint_.data()), bearer_token, exact_body, record_count};
  if (!valid_archive_http_request(request) || !configured_ || !connected() ||
      ca_certificate_ == nullptr) {
    return TransportResult::Failed;
  }

  if (state_mutex_ == nullptr) {
    state_mutex_ = xSemaphoreCreateMutex();
  }
  if (completion_ == nullptr) {
    completion_ = xSemaphoreCreateBinary();
  }
  if (state_mutex_ == nullptr || completion_ == nullptr ||
      xSemaphoreTake(state_mutex_, 0U) != pdTRUE) {
    return TransportResult::Failed;
  }
  if (task_state_ != TaskState::Idle) {
    xSemaphoreGive(state_mutex_);
    return TransportResult::Failed;
  }
  // A previous failed create must not leave a stale publication token for the
  // next request. Normal completion consumes this token in poll_response().
  (void)xSemaphoreTake(completion_, 0U);
  task_body_ = exact_body.data();
  task_body_size_ = exact_body.size();
  task_token_ = bearer_token.data();
  task_token_size_ = bearer_token.size();
  task_record_count_ = record_count;
  task_timeout_ms_ = timeout_ms;
  task_response_ = NetworkHttpResponse{};
  task_result_ = TransportResult::Failed;
  task_cancelled_ = false;
  task_state_ = TaskState::Running;
  xSemaphoreGive(state_mutex_);
  const auto created = xTaskCreate(
      &WiFiArchiveHttpTransport::network_task, "sigap_net",
      kTlsWorkerStackBytes, this,
      1U, nullptr);
  if (created != pdPASS) {
    if (xSemaphoreTake(state_mutex_, 0U) == pdTRUE) {
      task_state_ = TaskState::Idle;
      xSemaphoreGive(state_mutex_);
    }
    return TransportResult::Failed;
  }
  return TransportResult::Pending;
}

TransportResult WiFiArchiveHttpTransport::poll_response(
    NetworkHttpResponse& response) noexcept {
  if (completion_ == nullptr) return TransportResult::Failed;
  if (xSemaphoreTake(completion_, 0U) != pdTRUE) {
    if (state_mutex_ == nullptr) return TransportResult::Failed;
    // Publication and mutex contention must not retire an outstanding request.
    if (xSemaphoreTake(state_mutex_, 0U) != pdTRUE) return TransportResult::Pending;
    const auto state = task_state_;
    xSemaphoreGive(state_mutex_);
    return state == TaskState::Idle ? TransportResult::Failed
                                    : TransportResult::Pending;
  }
  if (state_mutex_ == nullptr) {
    (void)xSemaphoreGive(completion_);
    return TransportResult::Failed;
  }
  if (xSemaphoreTake(state_mutex_, 0U) != pdTRUE) {
    (void)xSemaphoreGive(completion_);
    return TransportResult::Pending;
  }
  if (task_state_ != TaskState::Complete) {
    xSemaphoreGive(state_mutex_);
    return TransportResult::Failed;
  }
  response = task_response_;
  const auto result = task_result_;
  task_state_ = TaskState::Idle;
  xSemaphoreGive(state_mutex_);
  return result;
}

void WiFiArchiveHttpTransport::cancel_batch() noexcept {
  if (state_mutex_ == nullptr ||
      xSemaphoreTake(state_mutex_, 0U) != pdTRUE) {
    return;
  }
  if (task_state_ == TaskState::Running) task_cancelled_ = true;
  xSemaphoreGive(state_mutex_);
}

void WiFiArchiveHttpTransport::network_task(void* context) noexcept {
  auto& self = *static_cast<WiFiArchiveHttpTransport*>(context);
  const auto result = self.post_batch(
      std::string_view(self.task_body_, self.task_body_size_),
      std::string_view(self.task_token_, self.task_token_size_),
      self.task_record_count_, self.task_timeout_ms_, self.task_response_);
  if (self.state_mutex_ != nullptr &&
      xSemaphoreTake(self.state_mutex_, portMAX_DELAY) == pdTRUE) {
    self.task_result_ = self.task_cancelled_ ? TransportResult::Failed : result;
    self.task_state_ = TaskState::Complete;
    xSemaphoreGive(self.state_mutex_);
  }
  if (self.completion_ != nullptr) (void)xSemaphoreGive(self.completion_);
  vTaskDelete(nullptr);
}

TransportResult WiFiArchiveHttpTransport::post_batch(
    std::string_view exact_body,
    std::string_view bearer_token,
    uint16_t record_count,
    uint32_t timeout_ms,
    NetworkHttpResponse& response) noexcept {
  response = NetworkHttpResponse{};
  const HttpPostRequest request{
      std::string_view(endpoint_.data()), bearer_token, exact_body, record_count};
  if (!valid_archive_http_request(request)) {
    log_http_diagnostic(1U, 0, record_count, 0U, false, false);
    return TransportResult::Failed;
  }
  const bool is_connected = configured_ && connected();
  if (!configured_ || !is_connected || ca_certificate_ == nullptr) {
    const int readiness = (configured_ ? 1 : 0) | (is_connected ? 2 : 0) |
                          (ca_certificate_ != nullptr ? 4 : 0);
    log_http_diagnostic(2U, readiness, record_count, 0U, false, false);
    return TransportResult::Failed;
  }

  std::array<char, kMaxAuthorizationBytes> authorization{};
  constexpr std::string_view prefix{"Bearer "};
  if (prefix.size() + bearer_token.size() >= authorization.size()) {
    log_http_diagnostic(1U, 0, record_count, 0U, false, false);
    return TransportResult::Failed;
  }
  std::copy(prefix.begin(), prefix.end(), authorization.begin());
  std::copy(bearer_token.begin(), bearer_token.end(),
            authorization.begin() + prefix.size());

  const uint32_t effective_timeout_ms = timeout_ms == 0U ? kHttpTimeoutMs :
      std::min<uint32_t>(timeout_ms, std::numeric_limits<uint16_t>::max());
  const uint32_t timeout_seconds = effective_timeout_ms / 1000U +
                                  (effective_timeout_ms % 1000U != 0U ? 1U : 0U);
  WiFiClientSecure client;
  client.setCACert(ca_certificate_);
  // Secure-client timeouts are seconds; HTTPClient timeouts are milliseconds.
  client.setTimeout(timeout_seconds);
  client.setHandshakeTimeout(timeout_seconds);
  HTTPClient http;
  if (!http.begin(client, endpoint_.data())) {
    log_http_diagnostic(3U, 0, record_count, 0U, false, false);
    return TransportResult::Failed;
  }
  http.setConnectTimeout(static_cast<int32_t>(effective_timeout_ms));
  http.setTimeout(static_cast<uint16_t>(effective_timeout_ms));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", authorization.data());
  const char* response_headers[] = {"Retry-After"};
  http.collectHeaders(response_headers, 1U);
  const int status = http.POST(
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(request.exact_body.data())),
      request.exact_body.size());
  response.status_code = status;
  log_http_diagnostic(4U, status, record_count, 0U, false, false);
  if (status <= 0) {
    http.end();
    return TransportResult::Timeout;
  }
  const String retry_after = http.header("Retry-After");
  response.retry_after_ms = parse_retry_after_ms(
      std::string_view(retry_after.c_str(), retry_after.length()));
  response_body_size_ = 0U;
  response.body_size = 0U;
  response.body_truncated = false;
  Stream* stream = http.getStreamPtr();
  if (stream != nullptr) {
    const uint32_t started_ms = millis();
    const int content_length = http.getSize();
    const bool has_content_length = content_length >= 0;
    const std::size_t expected_size = has_content_length
                                          ? static_cast<std::size_t>(content_length)
                                          : 0U;
    if (expected_size > response_body_.size()) response.body_truncated = true;
    while (!response.body_truncated &&
           millis() - started_ms < effective_timeout_ms) {
      if (has_content_length && response_body_size_ >= expected_size) break;
      if (response_body_size_ == response_body_.size()) {
        if (stream->available() > 0) response.body_truncated = true;
        if (has_content_length && expected_size == response_body_.size() &&
            stream->available() == 0) {
          break;
        }
        if (response.body_truncated) break;
      }
      const std::size_t available = static_cast<std::size_t>(stream->available());
      if (available == 0U) {
        delay(1U);
        continue;
      }
      const std::size_t remaining = has_content_length
                                        ? expected_size - response_body_size_
                                        : response_body_.size() -
                                              response_body_size_;
      const std::size_t amount = std::min(available, remaining);
      const int read = stream->readBytes(
          response_body_.data() + response_body_size_, amount);
      if (read <= 0) {
        delay(1U);
        continue;
      }
      response_body_size_ += static_cast<std::size_t>(read);
    }
    if (has_content_length && response_body_size_ < expected_size) {
      response.body_truncated = true;
    }
    std::copy(response_body_.begin(),
              response_body_.begin() + static_cast<std::ptrdiff_t>(response_body_size_),
              response.body.begin());
    response.body_size = response_body_size_;
    response.body_truncated = response.body_truncated || stream->available() > 0;
  }
  response.ack_present =
      status >= 200 && status < 300 &&
      decode_archive_ack(std::string_view(response_body_.data(), response_body_size_),
                         response.ack);
  log_http_diagnostic(5U, status, record_count, response.body_size,
                      response.ack_present, response.body_truncated);
  http.end();
  return TransportResult::Response;
}

#endif  // ARDUINO

}  // namespace sigap::network
