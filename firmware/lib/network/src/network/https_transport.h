#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "network/network.h"
#include "provisioning/secret_store.h"

namespace sigap::network {

inline constexpr std::size_t kMaxArchiveBodyBytes{32U * 1024U};
inline constexpr std::size_t kMaxArchiveRecords{128U};
inline constexpr std::size_t kMaxEndpointBytes{192U};
inline constexpr std::size_t kMaxResponseBodyBytes{512U};
inline constexpr std::size_t kMaxAuthorizationBytes{7U + 128U + 1U};
inline constexpr uint32_t kHttpTimeoutMs{15000U};

enum class HttpPostStatus : uint8_t {
  Delivered,
  Retryable,
  Unauthorized,
  Conflict,
  ProtocolError,
  Offline,
  InvalidRequest,
};

struct HttpPostRequest final {
  std::string_view endpoint{};
  std::string_view bearer_token{};
  std::string_view exact_body{};
  uint16_t record_count{0U};
};

bool decode_archive_ack(std::string_view body,
                        telemetry_archive::Ack& output) noexcept;

HttpPostStatus classify_http_status(int status_code) noexcept;
uint32_t parse_retry_after_ms(std::string_view value) noexcept;
bool valid_archive_http_request(const HttpPostRequest& request) noexcept;

#ifdef ARDUINO

class WiFiArchiveHttpTransport final : public IHttpTransport {
 public:
  WiFiArchiveHttpTransport() noexcept = default;

  // Starts station association without waiting for a connection. TLS is
  // rejected unless a CA bundle/certificate is supplied by the composition
  // root; insecure client mode is intentionally unavailable.
  bool set_endpoint(std::string_view endpoint) noexcept;
  bool begin(const provisioning::NetworkSecrets& secrets,
             const char* ca_certificate) noexcept;
  bool connected() const noexcept;
  TransportResult post_batch(std::string_view exact_body,
                             std::string_view bearer_token,
                             uint16_t record_count,
                             uint32_t timeout_ms,
                             NetworkHttpResponse& response) noexcept override;

 private:
  std::array<char, kMaxEndpointBytes + 1U> endpoint_{};
  std::array<char, kMaxResponseBodyBytes> response_body_{};
  std::size_t response_body_size_{0U};
  const char* ca_certificate_{nullptr};
  bool configured_{false};
};

#endif  // ARDUINO

}  // namespace sigap::network
