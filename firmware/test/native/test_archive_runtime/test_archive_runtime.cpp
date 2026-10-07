#include <cstdint>
#include <cstdio>
#include <cstring>

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

void test_sample_drain_archives_without_filesystem_in_callback() {
  std::printf("memory ArchiveRuntime=%zu ResultEnvelope=%zu\n",
              sizeof(ArchiveRuntime), sizeof(ProcessingRuntime::ResultEnvelope));
  storage::MemoryArchiveStore<4U> store(64U * 1024U);
  ArchiveRuntime runtime(&store);
  ArchiveRuntime::SampleHandoff handoff{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(handoff.try_push(sample())));
  TEST_ASSERT_TRUE(runtime.drain_samples(handoff));
  TEST_ASSERT_EQUAL_UINT32(0U, handoff.size());
  TEST_ASSERT_EQUAL_UINT64(1U, runtime.durable_records());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(storage::ArchiveHealth::Healthy),
                        static_cast<int>(runtime.health()));
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
}

void test_failed_sample_append_keeps_acquisition_handoff_for_retry() {
  storage::MemoryArchiveStore<4U> store(64U * 1024U);
  ArchiveRuntime runtime(&store);
  ArchiveRuntime::SampleHandoff handoff{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(handoff.try_push(sample())));
  store.set_fault(storage::StoreFault::ShortWrite);
  TEST_ASSERT_FALSE(runtime.drain_samples(handoff));
  TEST_ASSERT_EQUAL_UINT32(1U, handoff.size());
  TEST_ASSERT_EQUAL_UINT32(0U, store.size());
  TEST_ASSERT_EQUAL_UINT64(0U, runtime.gaps().unlogged_records());
  store.set_fault(storage::StoreFault::None);
  TEST_ASSERT_TRUE(runtime.drain_samples(handoff));
  TEST_ASSERT_EQUAL_UINT32(0U, handoff.size());
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

void test_result_codec_persists_fis_and_safety_evidence() {
  auto base = result();
  auto traced = result();
  auto fuzzy = traced.result.fuzzy.value();
  fuzzy.score_present = true;
  fuzzy.score = 42.5F;
  fuzzy.label_present = true;
  fuzzy.trace_present = true;
  fuzzy.trace.aggregate.present = true;
  fuzzy.trace.aggregate.area = 1.25F;
  auto safety = traced.result.safety.value();
  safety.trace.evidence_count = 1U;
  safety.trace.evidence[0U].value = 70.0F;
  traced.result = ProcessingRuntime::Result{
      traced.result.processing, fuzzy, safety};

  storage::MemoryArchiveStore<4U> base_store(64U * 1024U);
  storage::MemoryArchiveStore<4U> traced_store(64U * 1024U);
  ArchiveRuntime base_runtime(&base_store);
  ArchiveRuntime traced_runtime(&traced_store);
  ProcessingRuntime::Handoff base_handoff{};
  ProcessingRuntime::Handoff traced_handoff{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::ResultHandoffStatus::Enqueued),
                        static_cast<int>(base_handoff.try_push(base)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::ResultHandoffStatus::Enqueued),
                        static_cast<int>(traced_handoff.try_push(traced)));
  TEST_ASSERT_TRUE(base_runtime.drain_results(base_handoff));
  TEST_ASSERT_TRUE(traced_runtime.drain_results(traced_handoff));
  const auto& base_record = base_store.at(0U);
  const auto& traced_record = traced_store.at(0U);
  TEST_ASSERT_NOT_EQUAL(0, std::memcmp(base_record.bytes.data(),
                                       traced_record.bytes.data(),
                                       base_record.size));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sample_drain_archives_without_filesystem_in_callback);
  RUN_TEST(test_failed_sample_append_keeps_acquisition_handoff_for_retry);
  RUN_TEST(test_failed_result_append_keeps_handoff_for_retry);
  RUN_TEST(test_result_codec_persists_fis_and_safety_evidence);
  return UNITY_END();
}
