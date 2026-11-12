#include <array>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include "sigap_app.h"

namespace {

using sigap::network::DeliveryStatus;
using sigap::network::IHttpTransport;
using sigap::network::IMonotonicClock;
using sigap::network::IRandomJitter;
using sigap::network::IWifiLink;
using sigap::network::NetworkHttpResponse;
using sigap::network::NetworkOutcome;
using sigap::network::NetworkPolicy;
using sigap::network::NetworkWorker;
using sigap::network::TransportResult;
using sigap::telemetry_archive::Ack;
using sigap::telemetry_archive::AckStatus;
using sigap::telemetry_archive::ImmutableBatch;

struct RuntimeStub final {
  ImmutableBatch value{};
  bool has_pending{false};
  uint32_t applied{0U};

  bool pending() const noexcept { return has_pending; }
  const ImmutableBatch& batch() const noexcept { return value; }
  sigap::telemetry_archive::AckPersistStatus apply_ack(const Ack& ack) noexcept {
    if (!sigap::telemetry_archive::ack_matches(value, ack)) {
      return sigap::telemetry_archive::AckPersistStatus::Invalid;
    }
    ++applied;
    has_pending = false;
    return sigap::telemetry_archive::AckPersistStatus::Durable;
  }
};

struct Clock final : IMonotonicClock {
  uint64_t value{0U};
  uint64_t now_us() const noexcept override { return value; }
};

struct Jitter final : IRandomJitter {
  uint32_t sample(uint32_t) noexcept override { return 0U; }
};

struct Wifi final : IWifiLink {
  bool online{true};
  bool connected() const noexcept override { return online; }
  bool poll_connect() noexcept override { return online; }
};

struct Transport final : IHttpTransport {
  Ack ack{};
  bool valid_ack{true};
  uint32_t calls{0U};
  TransportResult post_batch(std::string_view body, std::string_view token,
                             uint16_t count, uint32_t,
                             NetworkHttpResponse& response) noexcept override {
    ++calls;
    response.status_code = 200;
    response.ack_present = valid_ack;
    response.ack = ack;
    TEST_ASSERT_EQUAL_UINT16(1U, count);
    TEST_ASSERT_EQUAL_STRING("token", token.data());
    TEST_ASSERT_TRUE(body.size() != 0U);
    return TransportResult::Response;
  }
};

NetworkPolicy policy() {
  NetworkPolicy value{};
  value.retry_min_ms = 100U;
  value.retry_max_ms = 1000U;
  value.request_timeout_ms = 1000U;
  value.retry_after_max_ms = 1000U;
  return value;
}

Ack valid_ack(const ImmutableBatch& batch) {
  Ack ack{};
  ack.status = AckStatus::Durable;
  ack.device_id = batch.device_id;
  ack.batch_id_size = batch.batch_id_size;
  std::copy(batch.batch_id.begin(),
            batch.batch_id.begin() + batch.batch_id_size,
            ack.batch_id.begin());
  ack.body_hash = batch.body_hash;
  ack.record_count = batch.record_count;
  ack.stored_count = batch.record_count;
  return ack;
}

void make_batch(RuntimeStub& runtime) {
  constexpr std::array<uint8_t, 2U> body{{'{', '}'}};
  TEST_ASSERT_TRUE(runtime.value.prepare("batch-1", body.data(), body.size(), 1U));
  runtime.value.device_id.present = true;
  runtime.value.device_id.bytes[0U] = 7U;
  runtime.has_pending = true;
}

void test_runtime_source_preserves_pending_until_valid_ack() {
  RuntimeStub runtime{};
  make_batch(runtime);
  sigap::ArchiveBatchNetworkSource<RuntimeStub> source(runtime);
  const ImmutableBatch* pending{nullptr};
  TEST_ASSERT_TRUE(source.peek_pending(pending));
  TEST_ASSERT_NOT_NULL(pending);
  const Ack wrong = valid_ack(runtime.value);
  Ack invalid = wrong;
  invalid.record_count = 2U;
  TEST_ASSERT_EQUAL(static_cast<int>(DeliveryStatus::Invalid),
                    static_cast<int>(source.acknowledge(invalid)));
  TEST_ASSERT_TRUE(runtime.has_pending);
  TEST_ASSERT_EQUAL(static_cast<int>(DeliveryStatus::Durable),
                    static_cast<int>(source.acknowledge(wrong)));
  TEST_ASSERT_FALSE(runtime.has_pending);
  TEST_ASSERT_EQUAL_UINT32(1U, runtime.applied);
}

void test_worker_offline_keeps_archive_pending_and_does_not_touch_runtime() {
  RuntimeStub runtime{};
  make_batch(runtime);
  sigap::ArchiveBatchNetworkSource<RuntimeStub> source(runtime);
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  wifi.online = false;
  Transport transport{};
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, source);
  TEST_ASSERT_TRUE(worker.begin("token"));
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Offline),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_TRUE(runtime.has_pending);
  TEST_ASSERT_EQUAL_UINT32(0U, runtime.applied);
  TEST_ASSERT_EQUAL_UINT32(0U, transport.calls);
}

void test_worker_delivers_only_matching_batch() {
  RuntimeStub runtime{};
  make_batch(runtime);
  sigap::ArchiveBatchNetworkSource<RuntimeStub> source(runtime);
  Clock clock{};
  Jitter jitter{};
  Wifi wifi{};
  Transport transport{};
  transport.ack = valid_ack(runtime.value);
  NetworkWorker worker(policy(), clock, jitter, wifi, transport, source);
  TEST_ASSERT_TRUE(worker.begin("token"));
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkOutcome::Delivered),
                    static_cast<int>(worker.poll()));
  TEST_ASSERT_FALSE(runtime.has_pending);
  TEST_ASSERT_EQUAL_UINT32(1U, runtime.applied);
}

void test_runtime_diagnostic_latch_emits_each_event_once() {
  sigap::RuntimeDiagnosticLatch latch{};
  TEST_ASSERT_TRUE(latch.first(sigap::RuntimeDiagnosticEvent::LoopAlive));
  TEST_ASSERT_FALSE(latch.first(sigap::RuntimeDiagnosticEvent::LoopAlive));
  TEST_ASSERT_TRUE(
      latch.first(sigap::RuntimeDiagnosticEvent::HttpsPollEntered));
  TEST_ASSERT_FALSE(
      latch.first(sigap::RuntimeDiagnosticEvent::HttpsPollEntered));
}

}  // namespace

void setup() {}
void loop() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_runtime_source_preserves_pending_until_valid_ack);
  RUN_TEST(test_worker_offline_keeps_archive_pending_and_does_not_touch_runtime);
  RUN_TEST(test_worker_delivers_only_matching_batch);
  RUN_TEST(test_runtime_diagnostic_latch_emits_each_event_once);
  return UNITY_END();
}
