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

void terminal_gap(void* context, const ArchiveIdentity& identity,
                  GapReason) noexcept {
  auto& log = *static_cast<TerminalLog*>(context);
  ++log.count;
  log.sequence = identity.sequence;
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

}  // namespace
