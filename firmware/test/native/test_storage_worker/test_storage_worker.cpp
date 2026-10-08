#include <cstdint>

#include <unity.h>

#include "storage/archive_codec.h"
#include "storage/archive_store.h"
#include "storage/storage_worker.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::storage;

DeviceId device() {
  DeviceId value{};
  value.present = true;
  value.bytes[0] = 0x31U;
  return value;
}

BootId boot() {
  BootId value{};
  value.present = true;
  value.bytes[0] = 0x42U;
  return value;
}

ImmutableRecordBytes record(uint64_t sequence) {
  SampleRecord sample{};
  sample.header.device_id = device();
  sample.header.boot_id = boot();
  sample.header.sequence = sequence;
  sample.header.sensor = SensorId::Mq4;
  sample.header.stream = StreamId::Organic;
  sample.header.kind = SampleKind::MqAdc;
  sample.header.monotonic_us = sequence * 1000U;
  sample.quality = SampleQuality::Ready;
  sample.reason = SampleReason::DataReady;
  sample.raw.present = true;
  sample.raw.adc_counts = 1234U;
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(sample, encoded));
  return encoded;
}

struct TerminalLog final {
  uint64_t count{0U};
  uint64_t sequence{0U};
};

struct CompletionLog final {
  uint64_t durable{0U};
  uint64_t terminal{0U};
  uint64_t last_sequence{0U};
};

void terminal_gap(void* context, const ArchiveIdentity& identity,
                  GapReason) noexcept {
  auto& log = *static_cast<TerminalLog*>(context);
  ++log.count;
  log.sequence = identity.sequence;
}

void completion(void* context, const ArchiveIdentity* identity,
                StorageWorkItem::CompletionStatus status) noexcept {
  auto& log = *static_cast<CompletionLog*>(context);
  if (status == StorageWorkItem::CompletionStatus::Durable) {
    ++log.durable;
  } else {
    ++log.terminal;
  }
  if (identity != nullptr) log.last_sequence = identity->sequence;
}

void test_retry_keeps_frozen_item_and_durable_once() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  StorageWorker<2U> worker(&store);
  const auto first = record(7U);
  TerminalLog log{};
  TEST_ASSERT_TRUE(worker.try_enqueue(first, &log, terminal_gap));
  store.set_fault(StoreFault::ShortWrite);
  TEST_ASSERT_FALSE(worker.process_one());
  TEST_ASSERT_EQUAL_UINT32(1U, worker.queue().size());
  TEST_ASSERT_EQUAL_UINT32(0U, store.size());
  store.set_fault(StoreFault::None);
  TEST_ASSERT_TRUE(worker.process_one());
  TEST_ASSERT_EQUAL_UINT32(0U, worker.queue().size());
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
  TEST_ASSERT_EQUAL_UINT64(0U, log.count);
  TEST_ASSERT_EQUAL_UINT64(1U, worker.stats().durable);
}

void test_queue_overflow_is_observable_without_overwrite() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  StorageWorker<2U> worker(&store);
  const auto first = record(1U);
  const auto second = record(2U);
  const auto third = record(3U);
  TEST_ASSERT_TRUE(worker.try_enqueue(first));
  TEST_ASSERT_TRUE(worker.try_enqueue(second));
  TEST_ASSERT_FALSE(worker.try_enqueue(third));
  TEST_ASSERT_EQUAL_UINT32(2U, worker.queue().size());
  TEST_ASSERT_EQUAL_UINT64(1U, worker.stats().overflow);
  TEST_ASSERT_EQUAL_UINT32(2U, worker.stats().high_water);
}

void test_changed_source_bytes_become_terminal_gap() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  StorageWorker<2U> worker(&store);
  auto first = record(11U);
  TerminalLog log{};
  TEST_ASSERT_TRUE(worker.try_enqueue(first, &log, terminal_gap));
  first.bytes[first.size - 1U] ^= 0x01U;
  TEST_ASSERT_TRUE(worker.process_one());
  TEST_ASSERT_EQUAL_UINT32(0U, worker.queue().size());
  TEST_ASSERT_EQUAL_UINT64(1U, log.count);
  TEST_ASSERT_EQUAL_UINT64(11U, log.sequence);
  TEST_ASSERT_EQUAL_UINT32(0U, store.size());
}

void test_budget_does_not_process_more_than_requested() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  StorageWorker<4U> worker(&store);
  const auto first = record(21U);
  const auto second = record(22U);
  TEST_ASSERT_TRUE(worker.try_enqueue(first));
  TEST_ASSERT_TRUE(worker.try_enqueue(second));
  TEST_ASSERT_EQUAL_UINT32(1U, worker.process(1U));
  TEST_ASSERT_EQUAL_UINT32(1U, worker.queue().size());
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
}

void test_owned_copy_survives_source_reuse_and_short_write() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  StorageWorker<2U> worker(&store);
  auto source = record(31U);
  const auto expected_hash = source.hash;
  TEST_ASSERT_TRUE(worker.try_enqueue_copy(source));
  source.bytes[0U] ^= 0xFFU;
  store.set_fault(StoreFault::ShortWrite);
  TEST_ASSERT_FALSE(worker.process_one());
  TEST_ASSERT_EQUAL_UINT32(1U, worker.queue().size());
  store.set_fault(StoreFault::None);
  TEST_ASSERT_TRUE(worker.process_one());
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
  TEST_ASSERT_EQUAL_UINT64(31U, store.at(0U).identity.sequence);
  TEST_ASSERT_TRUE(store.at(0U).hash == expected_hash);
}

void test_owned_slot_applies_backpressure_without_overwriting_pending() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  StorageWorker<4U> worker(&store);
  const auto first = record(41U);
  const auto second = record(42U);
  TEST_ASSERT_TRUE(worker.try_enqueue_copy(first));
  TEST_ASSERT_FALSE(worker.try_enqueue_copy(second));
  TEST_ASSERT_EQUAL_UINT32(1U, worker.queue().size());
  TEST_ASSERT_EQUAL_UINT64(1U, worker.stats().overflow);
  TEST_ASSERT_TRUE(worker.process_one());
  TEST_ASSERT_EQUAL_UINT64(41U, store.at(0U).identity.sequence);
}

void test_completion_ack_is_emitted_after_durable_append() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  StorageWorker<2U> worker(&store);
  CompletionLog log{};
  const auto first = record(51U);
  TEST_ASSERT_TRUE(worker.try_enqueue_copy(
      first, nullptr, nullptr, &log, completion));
  TEST_ASSERT_EQUAL_UINT64(0U, log.durable);
  TEST_ASSERT_TRUE(worker.process_one());
  TEST_ASSERT_EQUAL_UINT64(1U, log.durable);
  TEST_ASSERT_EQUAL_UINT64(51U, log.last_sequence);
}

void test_process_slice_stops_at_record_budget() {
  MemoryArchiveStore<8U> store(128U * 1024U);
  StorageWorker<4U> worker(&store);
  TEST_ASSERT_TRUE(worker.try_enqueue_copy(record(61U)));
  TEST_ASSERT_FALSE(worker.try_enqueue_copy(record(62U)));
  TEST_ASSERT_EQUAL_UINT32(1U, worker.process_slice(1U, 32U * 1024U));
  TEST_ASSERT_EQUAL_UINT32(0U, worker.queue().size());
  TEST_ASSERT_EQUAL_UINT64(1U, worker.stats().bytes_attempted > 0U);
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_retry_keeps_frozen_item_and_durable_once);
  RUN_TEST(test_queue_overflow_is_observable_without_overwrite);
  RUN_TEST(test_changed_source_bytes_become_terminal_gap);
  RUN_TEST(test_budget_does_not_process_more_than_requested);
  RUN_TEST(test_owned_copy_survives_source_reuse_and_short_write);
  RUN_TEST(test_owned_slot_applies_backpressure_without_overwriting_pending);
  RUN_TEST(test_completion_ack_is_emitted_after_durable_append);
  RUN_TEST(test_process_slice_stops_at_record_budget);
  return UNITY_END();
}
