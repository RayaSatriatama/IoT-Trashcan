#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include "network/network.h"

using sigap::network::DeliveryStatus;
using sigap::network::NetworkHttpResponse;
using sigap::network::IHttpTransport;
using sigap::network::IImmutableBatchSource;
using sigap::network::IAckDecoder;
using sigap::network::IMonotonicClock;
using sigap::network::IRandomJitter;
using sigap::network::IWifiLink;
using sigap::network::NetworkOutcome;
using sigap::network::NetworkPolicy;
using sigap::network::NetworkState;
using sigap::network::NetworkWorker;
using sigap::network::TransportResult;
using sigap::telemetry_archive::Ack;
using sigap::telemetry_archive::AckStatus;
using sigap::telemetry_archive::ImmutableBatch;

namespace {

struct Clock final : IMonotonicClock {
  uint64_t value{0U};
  uint64_t now_us() const noexcept override { return value; }
};

struct Jitter final : IRandomJitter {
  uint32_t value{0U};
  uint32_t sample(uint32_t maximum_ms) noexcept override {
    return value > maximum_ms ? maximum_ms : value;
  }
};

struct Wifi final : IWifiLink {
  bool online{true};
  uint32_t connect_calls{0U};
  bool connected() const noexcept override { return online; }
  bool poll_connect() noexcept override {
    ++connect_calls;
    return online;
  }
};

struct Transport final : IHttpTransport {
  TransportResult result{TransportResult::Response};
  int status{200};
  uint32_t retry_after{0U};
  Ack ack{};
  std::array<char, 32768> body{};
  std::size_t body_size{0U};
  uint32_t calls{0U};
  bool include_ack{true};
  std::string_view response_body{};
  uint32_t timeout_ms{0U};
  uint16_t record_count{0U};
  TransportResult post_batch(std::string_view bytes,
                             std::string_view token, uint16_t count,
                             uint32_t timeout,
                             NetworkHttpResponse& response) noexcept override {
    ++calls;
    timeout_ms = timeout;
    record_count = count;
    if (token != "token") return TransportResult::Failed;
    body_size = bytes.size();
    if (body_size > body.size()) return TransportResult::Failed;
    std::memcpy(body.data(), bytes.data(), body_size);
    response.status_code = status;
    response.retry_after_ms = retry_after;
    response.ack_present = status == 200 && include_ack;
    response.ack = ack;
    if (response_body.size() <= response.body.size()) {
      std::memcpy(response.body.data(), response_body.data(), response_body.size());
      response.body_size = response_body.size();
    }
    return result;
  }
};

struct Decoder final : IAckDecoder {
  bool valid{false};
  Ack value{};
  sigap::network::AckDecodeStatus decode(
      std::string_view body, Ack& output) noexcept override {
    if (!valid || body != "ack") {
      return sigap::network::AckDecodeStatus::Invalid;
    }
    output = value;
    return sigap::network::AckDecodeStatus::Valid;
  }
};

struct Batches final : IImmutableBatchSource {
  ImmutableBatch batch{};
  bool pending{false};
  uint32_t ack_calls{0U};
  DeliveryStatus ack_status{DeliveryStatus::Durable};
  bool peek_pending(const ImmutableBatch*& output) noexcept override {
    output = pending ? &batch : nullptr;
    return pending;
  }
  DeliveryStatus acknowledge(const Ack& ack) noexcept override {
    ++ack_calls;
    if (ack_status == DeliveryStatus::Durable) pending = false;
    return ack_status;
  }
};

NetworkPolicy policy() {
  NetworkPolicy value{};
  value.retry_min_ms = 1000U;
  value.retry_max_ms = 60000U;
  value.request_timeout_ms = 2500U;
  value.retry_after_max_ms = 60000U;
  value.jitter_max_ms = 100U;
  return value;
}

void make_batch(Batches& batches) {
  const std::array<uint8_t, 8> bytes{{'{', '"', 'x', '"', ':', '1', '}', '\n'}};
  TEST_ASSERT_TRUE(batches.batch.prepare("batch-1", bytes.data(), bytes.size(), 1U));
  sigap::acquisition::DeviceId device{};
  device.present = true;
  device.bytes[0] = 7U;
  batches.batch.set_device_id(device);
  batches.pending = true;
}

Ack valid_ack(const ImmutableBatch& batch) {
  Ack ack{};
  ack.status = AckStatus::Durable;
  ack.device_id = batch.device_id;
  ack.batch_id_size = batch.batch_id_size;
  std::copy(batch.batch_id.begin(), batch.batch_id.begin() + batch.batch_id_size,
            ack.batch_id.begin());
  ack.body_hash = batch.body_hash;
  ack.record_count = batch.record_count;
  ack.stored_count = batch.record_count;
  return ack;
}

void test_exact_body_is_retried_unchanged_and_acks() {
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  Transport transport{};
  Batches batches{};
  make_batch(batches);
  transport.ack = valid_ack(batches.batch);
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, batches);
  TEST_ASSERT_TRUE(worker.begin("token"));
  transport.result = TransportResult::Timeout;
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Timeout),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_TRUE(batches.pending);
  clock.value = worker.next_attempt_us();
  transport.result = TransportResult::Response;
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Delivered),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_FALSE(batches.pending);
  TEST_ASSERT_EQUAL_UINT32(2U, transport.calls);
  TEST_ASSERT_EQUAL_UINT32(8U, static_cast<uint32_t>(transport.body_size));
  TEST_ASSERT_EQUAL_UINT32(2500U, transport.timeout_ms);
  TEST_ASSERT_EQUAL_UINT16(1U, transport.record_count);
}

void test_offline_does_not_drop_batch_and_uses_backoff() {
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  wifi.online = false;
  Transport transport{};
  Batches batches{};
  make_batch(batches);
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, batches);
  TEST_ASSERT_TRUE(worker.begin("token"));
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Offline),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_EQUAL_UINT32(0U, transport.calls);
  TEST_ASSERT_TRUE(batches.pending);
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkState::Offline),
                    static_cast<int>(worker.state()));
}

void test_rate_limit_retry_after_is_bounded() {
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  Transport transport{};
  Batches batches{};
  make_batch(batches);
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, batches);
  TEST_ASSERT_TRUE(worker.begin("token"));
  transport.status = 429;
  transport.retry_after = 90000U;
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::RateLimited),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_EQUAL_UINT64(60000000ULL, worker.next_attempt_us());
  clock.value = worker.next_attempt_us();
  transport.status = 200;
  transport.ack = valid_ack(batches.batch);
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Delivered),
                    static_cast<int>(worker.poll()));
}

void test_ack_mismatch_never_marks_delivered() {
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  Transport transport{};
  Batches batches{};
  make_batch(batches);
  transport.ack = valid_ack(batches.batch);
  transport.ack.record_count = 2U;
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, batches);
  TEST_ASSERT_TRUE(worker.begin("token"));
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::ProtocolError),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_TRUE(batches.pending);
  TEST_ASSERT_EQUAL_UINT32(0U, batches.ack_calls);
}

void test_conflict_keeps_batch_pending() {
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  Transport transport{};
  Batches batches{};
  make_batch(batches);
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, batches);
  TEST_ASSERT_TRUE(worker.begin("token"));
  transport.status = 409;
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Conflict),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_TRUE(batches.pending);
  TEST_ASSERT_EQUAL_UINT32(0U, batches.ack_calls);
}

void test_server_errors_and_auth_are_observable() {
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  Transport transport{};
  Batches batches{};
  make_batch(batches);
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, batches);
  TEST_ASSERT_TRUE(worker.begin("token"));
  transport.status = 503;
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::ServerError),
                    static_cast<int>(worker.poll()));
  clock.value = worker.next_attempt_us();
  transport.status = 401;
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Unauthorized),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkState::AuthRequired),
                    static_cast<int>(worker.state()));
  TEST_ASSERT_TRUE(batches.pending);
}

void test_ack_decoder_is_bounded_and_strict() {
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  Transport transport{};
  Batches batches{};
  make_batch(batches);
  transport.include_ack = false;
  transport.response_body = "malformed";
  Decoder decoder{};
  decoder.value = valid_ack(batches.batch);
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, batches,
                        &decoder);
  TEST_ASSERT_TRUE(worker.begin("token"));
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::ProtocolError),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_TRUE(batches.pending);
  clock.value = worker.next_attempt_us();
  decoder.valid = true;
  transport.response_body = "ack";
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Delivered),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_FALSE(batches.pending);
}

}  // namespace

void setup() {}
void loop() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_exact_body_is_retried_unchanged_and_acks);
  RUN_TEST(test_offline_does_not_drop_batch_and_uses_backoff);
  RUN_TEST(test_rate_limit_retry_after_is_bounded);
  RUN_TEST(test_ack_mismatch_never_marks_delivered);
  RUN_TEST(test_conflict_keeps_batch_pending);
  RUN_TEST(test_server_errors_and_auth_are_observable);
  RUN_TEST(test_ack_decoder_is_bounded_and_strict);
  return UNITY_END();
}
