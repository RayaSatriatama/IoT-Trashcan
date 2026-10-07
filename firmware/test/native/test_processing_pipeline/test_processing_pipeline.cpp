#include <cstdint>

#include <unity.h>

#include "processing/processing_engine.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::processing;

ConfigRef config_ref() {
  ConfigRef ref{};
  ref.present = true;
  ref.schema_version = 1U;
  ref.config_version = 4U;
  ref.hash[0] = 0x11U;
  return ref;
}

SampleRecord sample(uint64_t sequence) {
  SampleRecord value{};
  value.header.device_id.present = true;
  value.header.boot_id.present = true;
  value.header.sequence = sequence;
  value.header.monotonic_us = 900U + sequence;
  value.header.stream = StreamId::Organic;
  value.header.sensor = SensorId::Hx711Organic;
  value.header.config_ref = config_ref();
  value.quality = SampleQuality::Uncalibrated;
  value.reason = SampleReason::InvalidProfile;
  return value;
}

void test_pipeline_preserves_missing_models_without_fabricating_outputs() {
  SampleWindow<4U> window{};
  auto value = sample(7U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(value)));

  ProcessingProfiles profiles{};
  ProcessingSnapshot snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingSnapshotStatus::Captured),
      static_cast<int>(ProcessingSnapshot::capture(
          1000U, 200U, StreamId::Organic, config_ref(), profiles, snapshot)));

  using Pipeline = ProcessingPipeline<4U, 4U>;
  Pipeline::Result result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(Pipeline::evaluate(window, snapshot, nullptr, nullptr,
                                          result)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisState::Unconfigured),
                        static_cast<int>(result.fuzzy.value().state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Unconfigured),
                        static_cast<int>(result.safety.value().state));
  TEST_ASSERT_EQUAL_UINT32(1U, result.processing.source_sample_ids.size());
  TEST_ASSERT_FALSE(result.processing.processed.weight_kg.available);
}

void test_pipeline_result_can_use_shared_sequence_factory() {
  SampleWindow<2U> window{};
  window.try_add(sample(8U));
  ProcessingProfiles profiles{};
  ProcessingSnapshot snapshot{};
  ProcessingSnapshot::capture(1000U, 200U, StreamId::Organic, config_ref(),
                              profiles, snapshot);
  using Pipeline = ProcessingPipeline<2U, 2U>;
  Pipeline::Result evaluated{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(Pipeline::evaluate(window, snapshot, nullptr, nullptr,
                                          evaluated)));

  SequenceAllocator sequence{};
  ResultRecordContext context{};
  context.device_id.present = true;
  context.boot_id.present = true;
  context.config_ref = config_ref();
  ResultRecordFactory factory(sequence, context);
  ResultEnvelope<fuzzy::FisResult, safety::SafetyResult, 2U> envelope{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ResultFactoryStatus::Produced),
      static_cast<int>(factory.make(1100U, evaluated, envelope)));
  TEST_ASSERT_EQUAL_UINT64(0U, envelope.header.sequence);
  TEST_ASSERT_EQUAL_UINT64(4U,
                           envelope.result.processing.config_ref.config_version);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pipeline_preserves_missing_models_without_fabricating_outputs);
  RUN_TEST(test_pipeline_result_can_use_shared_sequence_factory);
  return UNITY_END();
}
