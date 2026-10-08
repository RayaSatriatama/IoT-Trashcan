#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "telemetry_archive/immutable_batch.h"

namespace sigap::network {

// Network policy is an engineering policy snapshot. It is intentionally
// independent from the immutable batch bytes and from sensor acquisition.
struct NetworkPolicy final {
  uint32_t retry_min_ms{1000U};
  uint32_t retry_max_ms{60000U};
  uint32_t request_timeout_ms{10000U};
  uint32_t retry_after_max_ms{60000U};
  uint32_t jitter_max_ms{250U};

  constexpr bool valid() const noexcept {
    return retry_min_ms > 0U && retry_min_ms <= retry_max_ms &&
           request_timeout_ms > 0U && retry_after_max_ms > 0U;
  }
};

enum class NetworkState : uint8_t {
  Offline,
  Connecting,
  Online,
  Backoff,
  AuthRequired,
};

enum class NetworkOutcome : uint8_t {
  Idle,
  Waiting,
  Delivered,
  RetryScheduled,
  Offline,
  Timeout,
  RateLimited,
  ServerError,
  Unauthorized,
  Conflict,
  ProtocolError,
  InvalidBatch,
  DeliveryFailure,
};

enum class TransportResult : uint8_t { Response, Timeout, Failed };

inline constexpr std::size_t kMaxAckBodyBytes{512U};

struct NetworkHttpResponse final {
  int status_code{0};
  uint32_t retry_after_ms{0U};
  bool ack_present{false};
  telemetry_archive::Ack ack{};
  std::array<char, kMaxAckBodyBytes> body{};
  std::size_t body_size{0U};
  bool body_truncated{false};
};

enum class AckDecodeStatus : uint8_t { Valid, Invalid, TooLarge };

class IAckDecoder {
 public:
  virtual ~IAckDecoder() = default;
  // Decode only the bounded response body. The decoder must reject malformed
  // JSON, wrong fields, and non-archive ACK variants without side effects.
  virtual AckDecodeStatus decode(std::string_view body,
                                 telemetry_archive::Ack& output) noexcept = 0;
};

class IMonotonicClock {
 public:
  virtual ~IMonotonicClock() = default;
  virtual uint64_t now_us() const noexcept = 0;
};

class IRandomJitter {
 public:
  virtual ~IRandomJitter() = default;
  virtual uint32_t sample(uint32_t maximum_ms) noexcept = 0;
};

class IWifiLink {
 public:
  virtual ~IWifiLink() = default;
  virtual bool connected() const noexcept = 0;
  // A call is bounded and must not wait for association indefinitely.
  virtual bool poll_connect() noexcept = 0;
};

class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
  // body is an immutable view into the durable batch. Implementations must
  // send these bytes exactly, without reparsing or reserializing them.
  virtual TransportResult post_batch(std::string_view body,
                                     std::string_view device_token,
                                     uint16_t record_count,
                                     uint32_t timeout_ms,
                                     NetworkHttpResponse& response) noexcept = 0;
};

enum class DeliveryStatus : uint8_t {
  Durable,
  Retryable,
  Invalid,
};

class IImmutableBatchSource {
 public:
  virtual ~IImmutableBatchSource() = default;
  // The source owns the returned batch until the next poll. It must remain
  // pending until acknowledge() returns Durable.
  virtual bool peek_pending(
      const telemetry_archive::ImmutableBatch*& batch) noexcept = 0;
  virtual DeliveryStatus acknowledge(
      const telemetry_archive::Ack& ack) noexcept = 0;
};

class NetworkWorker final {
 public:
  NetworkWorker(NetworkPolicy policy, IMonotonicClock& clock,
                IRandomJitter& jitter, IWifiLink& wifi,
                IHttpTransport& transport,
                IImmutableBatchSource& batches,
                IAckDecoder* decoder = nullptr) noexcept
      : policy_(policy), clock_(clock), jitter_(jitter), wifi_(wifi),
        transport_(transport), batches_(batches), decoder_(decoder) {}

  bool begin(std::string_view device_token) noexcept {
    if (!policy_.valid() || device_token.empty() ||
        device_token.size() > kMaxTokenBytes) {
      state_ = NetworkState::AuthRequired;
      return false;
    }
    std::copy(device_token.begin(), device_token.end(), token_.begin());
    token_size_ = device_token.size();
    state_ = NetworkState::Offline;
    retry_attempt_ = 0U;
    next_attempt_us_ = 0U;
    return true;
  }

  NetworkOutcome poll() noexcept {
    const auto now = clock_.now_us();
    const telemetry_archive::ImmutableBatch* batch = nullptr;
    if (!batches_.peek_pending(batch)) return NetworkOutcome::Idle;
    if (batch == nullptr || !batch->attempted ||
        !batch->body_hash_matches() || batch->body_size == 0U) {
      state_ = NetworkState::Backoff;
      return NetworkOutcome::InvalidBatch;
    }
    if (now < next_attempt_us_) {
      state_ = NetworkState::Backoff;
      return NetworkOutcome::Waiting;
    }

    if (!wifi_.connected()) {
      state_ = NetworkState::Connecting;
      if (!wifi_.poll_connect() || !wifi_.connected()) {
        schedule_retry(now, 0U);
        state_ = NetworkState::Offline;
        return NetworkOutcome::Offline;
      }
    }
    state_ = NetworkState::Online;
    NetworkHttpResponse response{};
    const auto result = transport_.post_batch(
        batch->exact_body(), std::string_view(token_.data(), token_size_),
        batch->record_count, policy_.request_timeout_ms, response);
    if (result == TransportResult::Timeout) {
      schedule_retry(now, 0U);
      return NetworkOutcome::Timeout;
    }
    if (result == TransportResult::Failed) {
      schedule_retry(now, 0U);
      return NetworkOutcome::RetryScheduled;
    }

    if (response.status_code == 200) {
      if (!response.ack_present && decoder_ != nullptr &&
          response.body_size <= response.body.size() &&
          !response.body_truncated) {
        const auto decoded = decoder_->decode(
            std::string_view(response.body.data(), response.body_size),
            response.ack);
        response.ack_present = decoded == AckDecodeStatus::Valid;
      }
      if (!response.ack_present ||
          !telemetry_archive::ack_matches(*batch, response.ack)) {
        state_ = NetworkState::Backoff;
        schedule_retry(now, 0U);
        return NetworkOutcome::ProtocolError;
      }
      if (batches_.acknowledge(response.ack) != DeliveryStatus::Durable) {
        schedule_retry(now, 0U);
        return NetworkOutcome::DeliveryFailure;
      }
      retry_attempt_ = 0U;
      next_attempt_us_ = now;
      state_ = NetworkState::Online;
      return NetworkOutcome::Delivered;
    }

    if (response.status_code == 401 || response.status_code == 403) {
      state_ = NetworkState::AuthRequired;
      return NetworkOutcome::Unauthorized;
    }
    if (response.status_code == 409) {
      state_ = NetworkState::Backoff;
      return NetworkOutcome::Conflict;
    }
    if (response.status_code == 429) {
      schedule_retry(now, response.retry_after_ms);
      return NetworkOutcome::RateLimited;
    }
    if (response.status_code >= 500 && response.status_code <= 599) {
      schedule_retry(now, response.retry_after_ms);
      return NetworkOutcome::ServerError;
    }
    state_ = NetworkState::Backoff;
    return NetworkOutcome::ProtocolError;
  }

  NetworkState state() const noexcept { return state_; }
  uint32_t retry_attempt() const noexcept { return retry_attempt_; }
  uint64_t next_attempt_us() const noexcept { return next_attempt_us_; }

 private:
  static constexpr std::size_t kMaxTokenBytes{128U};

  void schedule_retry(uint64_t now_us, uint32_t retry_after_ms) noexcept {
    uint64_t delay_ms = retry_after_ms;
    if (delay_ms == 0U) {
      uint64_t exponential = policy_.retry_min_ms;
      const uint32_t shifts = std::min<uint32_t>(retry_attempt_, 31U);
      exponential <<= shifts;
      delay_ms = std::min<uint64_t>(exponential, policy_.retry_max_ms);
      const auto jitter = jitter_.sample(policy_.jitter_max_ms);
      delay_ms = std::min<uint64_t>(policy_.retry_max_ms,
                                    delay_ms + jitter);
    } else {
      delay_ms = std::min<uint64_t>(delay_ms, policy_.retry_after_max_ms);
    }
    delay_ms = std::max<uint64_t>(delay_ms, policy_.retry_min_ms);
    delay_ms = std::min<uint64_t>(delay_ms, policy_.retry_max_ms);
    const auto max_us = std::numeric_limits<uint64_t>::max();
    const auto delta_us =
        delay_ms > max_us / 1000U ? max_us : delay_ms * 1000U;
    next_attempt_us_ = now_us > max_us - delta_us ? max_us : now_us + delta_us;
    if (retry_attempt_ != std::numeric_limits<uint32_t>::max()) ++retry_attempt_;
    state_ = NetworkState::Backoff;
  }

  NetworkPolicy policy_{};
  IMonotonicClock& clock_;
  IRandomJitter& jitter_;
  IWifiLink& wifi_;
  IHttpTransport& transport_;
  IImmutableBatchSource& batches_;
  IAckDecoder* decoder_{nullptr};
  std::array<char, kMaxTokenBytes> token_{};
  std::size_t token_size_{0U};
  NetworkState state_{NetworkState::Offline};
  uint32_t retry_attempt_{0U};
  uint64_t next_attempt_us_{0U};
};

}  // namespace sigap::network
