#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <unity.h>

#include "archive_runtime.h"
#include "fuzzy/fis.h"
#include "processing_runtime.h"
#include "safety/safety.h"
#include "sigap/board/sd_transport.h"
#include "sigap_app.h"
#include "storage/archive_store.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::network;
using namespace sigap::storage;
using namespace sigap::telemetry_archive;

using BatchStore = MemoryImmutableBatchStore<4U>;
using AckJournal = MemoryAckJournalStore<4U>;
using BatchRuntime = ArchiveBatchRuntime<BatchStore, AckJournal>;

static_assert(board::sd_transport_policy_valid(board::sd_mmc_1bit_policy()),
              "onboard SD_MMC policy must remain a compile-time policy");
static_assert(board::sd_mmc_1bit_policy().clk == 39U &&
                  board::sd_mmc_1bit_policy().cmd_or_miso == 38U &&
                  board::sd_mmc_1bit_policy().d0_or_mosi == 40U &&
                  board::sd_mmc_1bit_policy().mode_1bit &&
                  !board::sd_mmc_1bit_policy().format_if_mount_failed,
              "SD_MMC policy must retain the PKM KC 1-bit pins and no-format rule");

DeviceId device() {
  DeviceId value{};
  value.present = true;
  value.bytes[0U] = 0x91U;
  return value;
}

BootId boot() {
  BootId value{};
  value.present = true;
  value.bytes[0U] = 0x92U;
  return value;
}

ConfigRef config_ref() {
  ConfigRef value{};
  value.present = true;
  value.schema_version = 1U;
  value.config_version = 9U;
  value.hash[0U] = 0x93U;
  return value;
}

SampleRecord synthetic_attempt(uint64_t sequence, uint64_t monotonic_us) {
  // TEST_ONLY synthetic boundary fixture. It is never routed to target I/O.
  SampleRecord value{};
  value.header.device_id = device();
  value.header.boot_id = boot();
  value.header.sequence = sequence;
  value.header.monotonic_us = monotonic_us;
  value.header.sensor = SensorId::Hx711Organic;
  value.header.stream = StreamId::Organic;
  value.header.kind = SampleKind::Hx711Counts;
  value.header.config_ref = config_ref();
  value.quality = SampleQuality::Uncalibrated;
  value.reason = SampleReason::ProfileMissing;
  return value;
}

processing::ResultRecordContext result_context() {
  processing::ResultRecordContext value{};
  value.device_id = device();
  value.boot_id = boot();
  value.config_ref = config_ref();
  return value;
}

void initialize_processing(ProcessingRuntime& processing,
                           SequenceAllocator& sequence,
                           TargetProfileSet& profiles) {
  auto active = config::kFactoryDefaults;
  TEST_ASSERT_TRUE(processing.begin(active, config_ref(), sequence,
                                    result_context(), profiles, 0U));
}

Ack ack_for(const ImmutableBatch& batch) {
  Ack value{};
  value.status = AckStatus::Durable;
  value.device_id = batch.device_id;
  value.batch_id_size = batch.batch_id_size;
  std::copy(batch.batch_id.begin(),
            batch.batch_id.begin() + batch.batch_id_size,
            value.batch_id.begin());
  value.body_hash = batch.body_hash;
  value.record_count = batch.record_count;
  value.stored_count = batch.record_count;
  return value;
}

struct Clock final : IMonotonicClock {
  uint64_t now{0U};
  uint64_t now_us() const noexcept override { return now; }
};

struct ZeroJitter final : IRandomJitter {
  uint32_t sample(uint32_t) noexcept override { return 0U; }
};

struct ConnectedWifi final : IWifiLink {
  bool connected() const noexcept override { return true; }
  bool poll_connect() noexcept override { return true; }
};

struct RetryingTransport final : IHttpTransport {
  Ack response_ack{};
  std::array<char, kMaxBatchBytes> first_body{};
  std::size_t first_size{0U};
  uint32_t start_calls{0U};
  uint32_t poll_calls{0U};
  bool reused_exact_bytes{true};

  TransportResult post_batch(std::string_view, std::string_view, uint16_t,
                             uint32_t, NetworkHttpResponse&) noexcept override {
    return TransportResult::Failed;
  }

  TransportResult start_batch(std::string_view body, std::string_view,
                              uint16_t, uint32_t,
                              NetworkHttpResponse&) noexcept override {
    ++start_calls;
    if (start_calls == 1U) {
      first_size = body.size();
      std::copy(body.begin(), body.end(), first_body.begin());
      return TransportResult::Failed;
    }
    reused_exact_bytes = first_size == body.size() &&
        std::equal(body.begin(), body.end(), first_body.begin());
    return TransportResult::Pending;
  }

  TransportResult poll_response(NetworkHttpResponse& response) noexcept override {
    ++poll_calls;
    response.status_code = 200;
    response.ack_present = true;
    response.ack = response_ack;
    return TransportResult::Response;
  }
};

struct InvalidAckTransport final : IHttpTransport {
  Ack response_ack{};
  uint32_t start_calls{0U};

  TransportResult post_batch(std::string_view, std::string_view, uint16_t,
                             uint32_t, NetworkHttpResponse&) noexcept override {
    return TransportResult::Failed;
  }

  TransportResult start_batch(std::string_view, std::string_view,
                              uint16_t, uint32_t,
                              NetworkHttpResponse& response) noexcept override {
    ++start_calls;
    response.status_code = 200;
    response.ack_present = true;
    response.ack = response_ack;
    return TransportResult::Response;
  }
};

NetworkPolicy network_policy() {
  NetworkPolicy value{};
  value.retry_min_ms = 10U;
  value.retry_max_ms = 100U;
  value.request_timeout_ms = 100U;
  value.retry_after_max_ms = 100U;
  value.jitter_max_ms = 0U;
  return value;
}

void drain_archive(ArchiveRuntime& runtime) {
  for (uint32_t attempt = 0U; attempt < 8U; ++attempt) {
    (void)runtime.poll_storage();
  }
}

void test_synthetic_attempt_reaches_processing_and_archive_without_handoff_theft() {
  MemoryArchiveStore<16U> archive_store(256U * 1024U);
  ArchiveRuntime archive(&archive_store);
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  ProcessingRuntime processing{};
  initialize_processing(processing, sequence, profiles);
  ArchiveRuntime::SampleHandoff acquisition_handoff{};
  const auto attempt = synthetic_attempt(41U, 900'000U);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(acquisition_handoff.try_push(attempt)));
  processing.observe(attempt);
  TEST_ASSERT_EQUAL_UINT32(1U, acquisition_handoff.size());
  TEST_ASSERT_EQUAL_UINT32(1U, processing.observed_samples());

  TEST_ASSERT_TRUE(archive.drain_samples(acquisition_handoff));
  TEST_ASSERT_EQUAL_UINT32(0U, acquisition_handoff.size());
  drain_archive(archive);
  TEST_ASSERT_EQUAL_UINT32(1U, archive_store.size());

  processing.poll(5'000'000U);
  TEST_ASSERT_TRUE(processing.handoff().size() > 0U);
  TEST_ASSERT_TRUE(archive.drain_results(processing.handoff()));
  drain_archive(archive);
  TEST_ASSERT_TRUE(archive_store.size() > 1U);
}

void test_batch_retry_keeps_exact_bytes_until_valid_ack_reclaims_bound_record() {
  MemoryArchiveStore<16U> archive_store(256U * 1024U);
  const auto attempt = synthetic_attempt(51U, 1'000U);
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(attempt, encoded));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive_store.append(encoded)));
  BatchStore batches(256U * 1024U);
  AckJournal journal{};
  BatchRuntime batch_runtime(&archive_store, &batches, &journal);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(batch_runtime.poll()));
  TEST_ASSERT_TRUE(batch_runtime.pending());
  const auto expected_body = batch_runtime.batch().exact_body();

  ArchiveBatchNetworkSource<BatchRuntime> source(batch_runtime);
  Clock clock{};
  ZeroJitter jitter{};
  ConnectedWifi wifi{};
  RetryingTransport transport{};
  transport.response_ack = ack_for(batch_runtime.batch());
  NetworkWorker worker(network_policy(), clock, jitter, wifi, transport, source);
  TEST_ASSERT_TRUE(worker.begin("test-only-token"));

  TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkOutcome::RetryScheduled),
                        static_cast<int>(worker.poll()));
  TEST_ASSERT_TRUE(batch_runtime.pending());
  TEST_ASSERT_EQUAL_UINT32(1U, transport.start_calls);
  TEST_ASSERT_EQUAL_MEMORY(expected_body.data(), transport.first_body.data(),
                           expected_body.size());

  clock.now = 10'000U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkOutcome::Waiting),
                        static_cast<int>(worker.poll()));
  TEST_ASSERT_EQUAL_UINT32(2U, transport.start_calls);
  TEST_ASSERT_TRUE(transport.reused_exact_bytes);
  TEST_ASSERT_TRUE(batch_runtime.pending());

  TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkOutcome::Delivered),
                        static_cast<int>(worker.poll()));
  TEST_ASSERT_EQUAL_UINT32(1U, transport.poll_calls);
  TEST_ASSERT_FALSE(batch_runtime.pending());
  ImmutableRecordBytes replay{};
  TEST_ASSERT_FALSE(archive_store.peek_pending(replay));
}

void test_malformed_ack_preserves_pending_batch_and_archive_record() {
  MemoryArchiveStore<16U> archive_store(256U * 1024U);
  const auto attempt = synthetic_attempt(61U, 1'000U);
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(attempt, encoded));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive_store.append(encoded)));
  BatchStore batches(256U * 1024U);
  AckJournal journal{};
  BatchRuntime batch_runtime(&archive_store, &batches, &journal);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(batch_runtime.poll()));

  ArchiveBatchNetworkSource<BatchRuntime> source(batch_runtime);
  Clock clock{};
  ZeroJitter jitter{};
  ConnectedWifi wifi{};
  InvalidAckTransport transport{};
  transport.response_ack = ack_for(batch_runtime.batch());
  ++transport.response_ack.record_count;
  NetworkWorker worker(network_policy(), clock, jitter, wifi, transport, source);
  TEST_ASSERT_TRUE(worker.begin("test-only-token"));

  TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkOutcome::ProtocolError),
                        static_cast<int>(worker.poll()));
  TEST_ASSERT_TRUE(batch_runtime.pending());
  ImmutableRecordBytes replay{};
  TEST_ASSERT_TRUE(archive_store.peek_pending(replay));
  TEST_ASSERT_EQUAL_UINT64(61U, replay.identity.sequence);
}

void test_storage_slice_and_pending_network_poll_are_bounded() {
  MemoryArchiveStore<16U> archive_store(256U * 1024U);
  ArchiveRuntime archive(&archive_store);
  ArchiveRuntime::SampleHandoff handoff{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(handoff.try_push(synthetic_attempt(71U, 1U))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(handoff.try_push(synthetic_attempt(72U, 2U))));
  TEST_ASSERT_TRUE(archive.drain_samples(handoff));
  // ArchiveRuntime owns one immutable worker slot. The second producer item
  // remains in the acquisition ring until the bounded worker poll completes.
  TEST_ASSERT_EQUAL_UINT32(1U, archive.storage_worker().queue().size());
  TEST_ASSERT_EQUAL_UINT32(1U, handoff.size());
  TEST_ASSERT_EQUAL_UINT32(1U, archive.poll_storage_slice(1U, 32U * 1024U));
  TEST_ASSERT_EQUAL_UINT32(0U, archive.storage_worker().queue().size());
  TEST_ASSERT_TRUE(archive.drain_samples(handoff));
  TEST_ASSERT_EQUAL_UINT32(1U, archive.storage_worker().queue().size());

  const auto attempt = synthetic_attempt(73U, 3U);
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(attempt, encoded));
  MemoryArchiveStore<4U> batch_archive(128U * 1024U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(batch_archive.append(encoded)));
  BatchStore batches(128U * 1024U);
  AckJournal journal{};
  BatchRuntime batch_runtime(&batch_archive, &batches, &journal);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(batch_runtime.poll()));
  ArchiveBatchNetworkSource<BatchRuntime> source(batch_runtime);
  Clock clock{};
  ZeroJitter jitter{};
  ConnectedWifi wifi{};
  RetryingTransport transport{};
  NetworkWorker worker(network_policy(), clock, jitter, wifi, transport, source);
  TEST_ASSERT_TRUE(worker.begin("test-only-token"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkOutcome::RetryScheduled),
                        static_cast<int>(worker.poll()));
  TEST_ASSERT_EQUAL_UINT32(1U, transport.start_calls);
  TEST_ASSERT_EQUAL_UINT32(0U, transport.poll_calls);
}

void test_sd_mmc_policy_is_compile_time_only_and_never_formats() {
  constexpr auto policy = board::sd_mmc_1bit_policy();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(board::SdTransport::SdMmc1Bit),
                        static_cast<int>(policy.transport));
  TEST_ASSERT_EQUAL_UINT8(39U, policy.clk);
  TEST_ASSERT_EQUAL_UINT8(38U, policy.cmd_or_miso);
  TEST_ASSERT_EQUAL_UINT8(40U, policy.d0_or_mosi);
  TEST_ASSERT_EQUAL_UINT8(0U, policy.cs);
  TEST_ASSERT_TRUE(policy.mode_1bit);
  TEST_ASSERT_FALSE(policy.format_if_mount_failed);
}

}  // namespace

void setup() {}
void loop() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_synthetic_attempt_reaches_processing_and_archive_without_handoff_theft);
  RUN_TEST(test_batch_retry_keeps_exact_bytes_until_valid_ack_reclaims_bound_record);
  RUN_TEST(test_malformed_ack_preserves_pending_batch_and_archive_record);
  RUN_TEST(test_storage_slice_and_pending_network_poll_are_bounded);
  RUN_TEST(test_sd_mmc_policy_is_compile_time_only_and_never_formats);
  return UNITY_END();
}
