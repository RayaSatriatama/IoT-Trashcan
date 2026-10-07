#include <cstdint>
#include <cstdio>

#include <unity.h>

#include "../../../src/app/processing_runtime.h"
#include "acquisition/handoff.h"
#include "models/model_lifecycle.h"
#include "sigap/config/config_defaults.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;

ConfigRef active_config_ref() {
  ConfigRef ref{};
  ref.present = true;
  ref.schema_version = 1U;
  ref.config_version = 7U;
  ref.hash[0] = 0xA7U;
  return ref;
}

DeviceId device_id() {
  DeviceId id{};
  id.present = true;
  id.bytes[0] = 0xD1U;
  return id;
}

BootId boot_id() {
  BootId id{};
  id.present = true;
  id.bytes[0] = 0xB1U;
  return id;
}

SampleRecord organic_attempt(uint64_t sequence, uint64_t monotonic_us) {
  SampleRecord record{};
  record.header.device_id = device_id();
  record.header.boot_id = boot_id();
  record.header.sequence = sequence;
  record.header.monotonic_us = monotonic_us;
  record.header.sensor = SensorId::Hx711Organic;
  record.header.stream = StreamId::Organic;
  record.header.kind = SampleKind::Hx711Counts;
  record.header.config_ref = active_config_ref();
  record.quality = SampleQuality::Uncalibrated;
  record.reason = SampleReason::ProfileMissing;
  return record;
}

struct CountingObserver final : ISampleObserver {
  void observe(const SampleRecord&) noexcept override { ++count; }

  uint32_t count{0U};
};

void initialize_runtime(ProcessingRuntime& runtime,
                        SequenceAllocator& sequence,
                        TargetProfileSet& profiles,
                        CountingObserver& downstream) {
  config::AppConfig active = config::kFactoryDefaults;
  active.telemetry.summary_period_ms = 5000U;
  processing::ResultRecordContext context{};
  context.device_id = device_id();
  context.boot_id = boot_id();
  context.config_ref = active_config_ref();
  runtime.set_downstream_observer(&downstream);
  TEST_ASSERT_TRUE(runtime.begin(active, active_config_ref(), sequence, context,
                                 profiles, 0U));
}

void test_runtime_observes_sample_without_consuming_acquisition_handoff() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  SampleRing<2U> acquisition_handoff{};
  const auto record = organic_attempt(11U, 100U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(acquisition_handoff.try_push(record)));

  runtime.observe(record);

  TEST_ASSERT_EQUAL_UINT32(1U, downstream.count);
  TEST_ASSERT_EQUAL_UINT32(1U, acquisition_handoff.size());
  TEST_ASSERT_EQUAL_UINT32(1U, runtime.observed_samples());
}

void test_runtime_cycle_emits_unconfigured_result_with_shared_provenance() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  runtime.observe(organic_attempt(21U, 100U));
  runtime.poll(5000000U);

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_EQUAL_UINT64(0U, result.header.sequence);
  TEST_ASSERT_EQUAL_UINT64(7U, result.header.config_ref.config_version);
  TEST_ASSERT_EQUAL_UINT32(1U,
                            result.result.processing.source_sample_ids.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisState::Unconfigured),
                        static_cast<int>(result.result.fuzzy.value().state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Unconfigured),
                        static_cast<int>(result.result.safety.value().state));
}

void test_runtime_full_result_handoff_is_observable_and_preserves_old_result() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  runtime.observe(organic_attempt(31U, 100U));
  runtime.poll(5000000U);
  runtime.observe(organic_attempt(32U, 5000100U));
  runtime.poll(10000000U);
  runtime.observe(organic_attempt(33U, 10000100U));
  runtime.poll(15000000U);
  runtime.observe(organic_attempt(34U, 15000100U));
  runtime.poll(20000000U);

  TEST_ASSERT_EQUAL_UINT32(3U, runtime.handoff().size());
  TEST_ASSERT_EQUAL_UINT64(1U, runtime.handoff().dropped_results());

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_EQUAL_UINT64(0U, result.header.sequence);
  TEST_ASSERT_EQUAL_UINT64(31U,
                           result.result.processing.source_sample_ids.at(0U)->sequence);
}

void test_processing_resource_footprint_is_bounded() {
  std::printf("memory ProcessingRuntime=%zu ProcessingSnapshot=%zu "
              "ProcessedValues=%zu ResultEnvelope=%zu ModelLifecycle=%zu\\n",
              sizeof(ProcessingRuntime), sizeof(processing::ProcessingSnapshot),
              sizeof(processing::ProcessedValues),
              sizeof(ProcessingRuntime::ResultEnvelope),
              sizeof(models::ModelProfileLifecycle));
  TEST_ASSERT_TRUE(sizeof(ProcessingRuntime) < 20000U);
  TEST_ASSERT_TRUE(sizeof(ProcessingRuntime::Handoff) < 20000U);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_runtime_observes_sample_without_consuming_acquisition_handoff);
  RUN_TEST(test_runtime_cycle_emits_unconfigured_result_with_shared_provenance);
  RUN_TEST(test_runtime_full_result_handoff_is_observable_and_preserves_old_result);
  RUN_TEST(test_processing_resource_footprint_is_bounded);
  return UNITY_END();
}
