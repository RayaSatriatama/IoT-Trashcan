#include <cstdint>
#include <cstdio>

#include <unity.h>

#include "../../../src/app/archive_runtime.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;

DeviceId device() {
  DeviceId value{};
  value.present = true;
  value.bytes[0] = 0xA1U;
  return value;
}

BootId boot() {
  BootId value{};
  value.present = true;
  value.bytes[0] = 0xB2U;
  return value;
}

ConfigRef config_ref() {
  ConfigRef value{};
  value.present = true;
  value.schema_version = 1U;
  value.config_version = 4U;
  value.hash[0] = 0x44U;
  return value;
}

SampleRecord sample() {
  SampleRecord value{};
  value.header.device_id = device();
  value.header.boot_id = boot();
  value.header.sequence = 10U;
  value.header.sensor = SensorId::Mq4;
  value.header.stream = StreamId::Organic;
  value.header.kind = SampleKind::MqAdc;
  value.header.config_ref = config_ref();
  value.quality = SampleQuality::Timeout;
  value.reason = SampleReason::Timeout;
  return value;
}

struct CountingObserver final : ISampleObserver {
  void observe(const SampleRecord&) noexcept override { ++count; }
  uint32_t count{0U};
};

ProcessingRuntime::ResultEnvelope result() {
  ProcessingRuntime::ResultEnvelope value{};
  value.header.device_id = device();
  value.header.boot_id = boot();
  value.header.sequence = 20U;
  value.header.config_ref = config_ref();
  value.result.processing.stream = StreamId::Organic;
  value.result.processing.computed_monotonic_us = 200U;
  value.result.processing.config_ref = config_ref();
  processing::SourceSampleId source{};
  source.device_id = device();
  source.boot_id = boot();
  source.sequence = 10U;
  (void)value.result.processing.source_sample_ids.try_add(source);
  return value;
}

void test_sample_observer_archives_and_forwards() {
  std::printf("memory ArchiveRuntime=%zu ResultEnvelope=%zu\n",
              sizeof(ArchiveRuntime), sizeof(ProcessingRuntime::ResultEnvelope));
  storage::MemoryArchiveStore<4U> store(64U * 1024U);
  ArchiveRuntime runtime(&store);
  CountingObserver observer{};
  runtime.set_secondary_observer(&observer);
  runtime.observe(sample());
  TEST_ASSERT_EQUAL_UINT32(1U, observer.count);
  TEST_ASSERT_EQUAL_UINT64(1U, runtime.durable_records());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(storage::ArchiveHealth::Healthy),
                        static_cast<int>(runtime.health()));
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
}

void test_failed_result_append_keeps_handoff_for_retry() {
  storage::MemoryArchiveStore<4U> store(64U * 1024U);
  ArchiveRuntime runtime(&store);
  ProcessingRuntime::Handoff handoff{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Enqueued),
      static_cast<int>(handoff.try_push(result())));
  store.set_fault(storage::StoreFault::ShortWrite);
  TEST_ASSERT_FALSE(runtime.drain_results(handoff));
  TEST_ASSERT_EQUAL_UINT32(1U, handoff.size());
  TEST_ASSERT_EQUAL_UINT32(0U, store.size());
  store.set_fault(storage::StoreFault::None);
  TEST_ASSERT_TRUE(runtime.drain_results(handoff));
  TEST_ASSERT_EQUAL_UINT32(0U, handoff.size());
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sample_observer_archives_and_forwards);
  RUN_TEST(test_failed_result_append_keeps_handoff_for_retry);
  return UNITY_END();
}
