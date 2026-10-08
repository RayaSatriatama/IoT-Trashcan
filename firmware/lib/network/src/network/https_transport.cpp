#include "https_transport.h"

#include <algorithm>
#include <cctype>
#include <limits>

#ifdef ARDUINO
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

namespace sigap::network {
namespace {

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

bool find_field(std::string_view body, std::string_view name,
                std::size_t& value_begin, std::size_t& value_end) noexcept {
  std::size_t key{std::string_view::npos};
  std::size_t cursor{0U};
  while ((cursor = body.find(name, cursor)) != std::string_view::npos) {
    const bool quoted = cursor > 0U && cursor + name.size() < body.size() &&
                        body[cursor - 1U] == '"' &&
                        body[cursor + name.size()] == '"';
    if (quoted) {
      if (key != std::string_view::npos) return false;
      key = cursor;
    }
    cursor += name.size();
  }
  if (key == std::string_view::npos) {
    return false;
  }
  cursor = key + name.size() + 1U;
  while (cursor < body.size() &&
         std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
    ++cursor;
  }
  if (cursor == body.size() || body[cursor] != ':') return false;
  ++cursor;
  while (cursor < body.size() &&
         std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
    ++cursor;
  }
  if (cursor == body.size()) return false;
  if (body[cursor] == '"') {
    value_begin = ++cursor;
    while (cursor < body.size() && body[cursor] != '"') {
      if (body[cursor] == '\\') return false;
      ++cursor;
    }
    if (cursor == body.size()) return false;
    value_end = cursor;
    return true;
  }
  value_begin = cursor;
  while (cursor < body.size() && body[cursor] != ',' && body[cursor] != '}') {
    ++cursor;
  }
  value_end = cursor;
  while (value_end > value_begin &&
         std::isspace(static_cast<unsigned char>(body[value_end - 1U])) != 0) {
    --value_end;
  }
  return value_begin != value_end;
}

bool decode_uint16(std::string_view body, std::string_view name,
                   uint16_t& output) noexcept {
  std::size_t begin{0U};
  std::size_t end{0U};
  if (!find_field(body, name, begin, end) || begin == end) return false;
  uint32_t value{0U};
  for (std::size_t index = begin; index < end; ++index) {
    const auto byte = static_cast<unsigned char>(body[index]);
    if (byte < '0' || byte > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(byte - '0');
    if (value > (std::numeric_limits<uint16_t>::max() - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  output = static_cast<uint16_t>(value);
  return true;
}

bool decode_string(std::string_view body, std::string_view name,
                   char* output, std::size_t capacity,
                   std::size_t& output_size) noexcept {
  std::size_t begin{0U};
  std::size_t end{0U};
  if (!find_field(body, name, begin, end) || end - begin >= capacity) return false;
  std::copy(body.begin() + static_cast<std::ptrdiff_t>(begin),
            body.begin() + static_cast<std::ptrdiff_t>(end), output);
  output_size = end - begin;
  output[output_size] = '\0';
  return true;
}

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
  char device[telemetry_archive::kMaxBatchIdBytes]{};
  char batch[telemetry_archive::kMaxBatchIdBytes]{};
  char hash[telemetry_archive::kMaxBatchIdBytes]{};
  char status[16]{};
  std::size_t device_size{0U};
  std::size_t batch_size{0U};
  std::size_t hash_size{0U};
  std::size_t status_size{0U};
  if (!decode_string(body, "device_id", device, sizeof(device), device_size) ||
      !decode_string(body, "batch_id", batch, sizeof(batch), batch_size) ||
      !decode_string(body, "body_hash", hash, sizeof(hash), hash_size) ||
      !decode_string(body, "status", status, sizeof(status), status_size) ||
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
  if (!decode_uint16(body, "record_count", output.record_count) ||
      !decode_uint16(body, "stored_count", output.stored_count) ||
      !decode_uint16(body, "deduplicated_count", output.deduplicated_count) ||
      output.record_count == 0U || output.record_count > kMaxArchiveRecords ||
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

TransportResult WiFiArchiveHttpTransport::post_batch(
    std::string_view exact_body,
    std::string_view bearer_token,
    uint16_t record_count,
    uint32_t timeout_ms,
    NetworkHttpResponse& response) noexcept {
  response = NetworkHttpResponse{};
  const HttpPostRequest request{
      std::string_view(endpoint_.data()), bearer_token, exact_body, record_count};
  if (!valid_archive_http_request(request)) return TransportResult::Failed;
  if (!configured_ || !connected() || ca_certificate_ == nullptr) {
    return TransportResult::Failed;
  }

  std::array<char, kMaxAuthorizationBytes> authorization{};
  constexpr std::string_view prefix{"Bearer "};
  if (prefix.size() + bearer_token.size() >= authorization.size()) {
    return TransportResult::Failed;
  }
  std::copy(prefix.begin(), prefix.end(), authorization.begin());
  std::copy(bearer_token.begin(), bearer_token.end(),
            authorization.begin() + prefix.size());

  WiFiClientSecure client;
  client.setCACert(ca_certificate_);
  client.setTimeout(kHttpTimeoutMs);
  HTTPClient http;
  if (!http.begin(client, endpoint_.data())) {
    return TransportResult::Failed;
  }
  http.setTimeout(timeout_ms == 0U ? kHttpTimeoutMs : timeout_ms);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", authorization.data());
  const char* response_headers[] = {"Retry-After"};
  http.collectHeaders(response_headers, 1U);
  const int status = http.POST(
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(request.exact_body.data())),
      request.exact_body.size());
  response.status_code = status;
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
    while (stream->available() > 0) {
      if (response_body_size_ == response_body_.size()) {
        break;
      }
      const std::size_t available = static_cast<std::size_t>(stream->available());
      const std::size_t capacity = response_body_.size() - response_body_size_;
      const std::size_t amount = std::min(available, capacity);
      const int read = stream->readBytes(response_body_.data() + response_body_size_,
                                         amount);
      if (read <= 0) break;
      response_body_size_ += static_cast<std::size_t>(read);
    }
    std::copy(response_body_.begin(),
              response_body_.begin() + static_cast<std::ptrdiff_t>(response_body_size_),
              response.body.begin());
    response.body_size = response_body_size_;
    response.body_truncated = stream->available() > 0;
  }
  response.ack_present =
      status >= 200 && status < 300 &&
      decode_archive_ack(std::string_view(response_body_.data(), response_body_size_),
                         response.ack);
  http.end();
  return TransportResult::Response;
}

#endif  // ARDUINO

}  // namespace sigap::network
