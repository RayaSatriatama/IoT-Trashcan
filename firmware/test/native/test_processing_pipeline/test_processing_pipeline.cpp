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

SampleRecord ds_sample(uint64_t sequence, uint64_t monotonic_us,
                       int32_t temperature_centi_c) {
  auto value = sample(sequence);
  value.header.sensor = SensorId::Ds18b20B3;
  value.header.stream = StreamId::B3;
  value.header.kind = SampleKind::Ds18b20;
  value.header.monotonic_us = monotonic_us;
  value.quality = SampleQuality::Ready;
  value.reason = SampleReason::DataReady;
  value.raw.present = true;
  value.raw.identity_valid = true;
  value.raw.conversion_complete = true;
  value.raw.crc_valid = true;
  value.raw.temperature_centi_c = temperature_centi_c;
  return value;
}

SampleRecord mq_sample(uint64_t sequence, SensorId sensor, float millivolts) {
  auto value = sample(sequence);
  value.header.sensor = sensor;
  value.header.stream = StreamId::B3;
  value.header.kind = SampleKind::MqAdc;
  value.quality = SampleQuality::Ready;
  value.reason = SampleReason::DataReady;
  value.raw.present = true;
  value.converted.primary_available = true;
  value.converted.primary_kind = ConvertedKind::AdcMillivolts;
  value.converted.primary_value = millivolts;
  return value;
}

models::SafetyModelProfile safety_fixture() {
  models::SafetyModelProfile profile{};
  profile.manifest.kind = ProfileKind::Safety;
  profile.manifest.schema_version = 1U;
  profile.manifest.profile_version = 1U;
  profile.manifest.target_id = static_cast<uint32_t>(models::ModelTarget::SafetyB3);
  profile.manifest.validity = ProfileValidity::TestOnly;
  profile.deployment = models::ModelDeployment::TestOnly;
  profile.channel_count = 1U;
  profile.channels[0] = {models::HazardSignal::TemperatureC, true, 60.0F,
                         50.0F};
  profile.clear_consecutive_cycles = 2U;
  return profile;
}

models::SafetyModelProfile mq_safety_fixture() {
  auto profile = safety_fixture();
  profile.channel_count = 2U;
  profile.channels[0] = {models::HazardSignal::Mq7AdcMillivolts, true, 2000.0F,
                         1500.0F};
  profile.channels[1] = {models::HazardSignal::Mq8AdcMillivolts, true, 2000.0F,
                         1500.0F};
  return profile;
}

MqAdcProfile mq_profile(ProfileTarget target) {
  MqAdcProfile profile{};
  profile.manifest.kind = ProfileKind::Adc;
  profile.manifest.schema_version = 1U;
  profile.manifest.profile_version = 1U;
  profile.manifest.target_id = static_cast<uint32_t>(target);
  profile.manifest.validity = ProfileValidity::TestOnly;
  profile.manifest.hash[0] = static_cast<uint8_t>(target);
  profile.reference_mv = 3300U;
  profile.full_scale_counts = 4095U;
  profile.divider_numerator = 1U;
  profile.divider_denominator = 1U;
  return profile;
}

Ds18b20Profile ds_profile() {
  Ds18b20Profile profile{};
  profile.manifest.kind = ProfileKind::Calibration;
  profile.manifest.schema_version = 1U;
  profile.manifest.profile_version = 1U;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  profile.manifest.validity = ProfileValidity::TestOnly;
  profile.has_rom = true;
  profile.rom[0] = 0x28U;
  profile.resolution_bits = 12U;
  profile.manifest.hash[0] = 0x32U;
  return profile;
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
  safety::SafetyEvaluator safety_evaluator{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(Pipeline::evaluate(window, snapshot, nullptr, nullptr,
                                          safety_evaluator, result)));
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
  safety::SafetyEvaluator safety_evaluator{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(Pipeline::evaluate(window, snapshot, nullptr, nullptr,
                                          safety_evaluator, evaluated)));

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

void test_pipeline_preserves_b3_safety_latch_across_other_streams() {
  const auto temperature_profile = ds_profile();
  const auto safety_profile = safety_fixture();
  ProcessingProfiles profiles{};
  profiles.ds18b20 = temperature_profile;
  profiles.ds18b20_present = true;
  profiles.temperature_source = TemperatureSource::Ds18b20B3;

  SampleWindow<4U> b3_window{};
  auto hot_sample = ds_sample(20U, 900U, 7000);
  hot_sample.header.profile_ref = temperature_profile.ref();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(b3_window.try_add(hot_sample)));
  ProcessingSnapshot b3_snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingSnapshotStatus::Captured),
      static_cast<int>(ProcessingSnapshot::capture(
          1000U, 200U, StreamId::B3, config_ref(), profiles, b3_snapshot)));

  using Pipeline = ProcessingPipeline<4U, 4U>;
  Pipeline::Result result{};
  safety::SafetyEvaluator evaluator{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(Pipeline::evaluate(b3_window, b3_snapshot, nullptr,
                                          &safety_profile, evaluator, result)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Alert),
                        static_cast<int>(result.safety.value().state));

  SampleWindow<4U> organic_window{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(organic_window.try_add(sample(21U))));
  ProcessingSnapshot organic_snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingSnapshotStatus::Captured),
      static_cast<int>(ProcessingSnapshot::capture(
          1000U, 200U, StreamId::Organic, config_ref(), ProcessingProfiles{},
          organic_snapshot)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(Pipeline::evaluate(organic_window, organic_snapshot,
                                          nullptr, nullptr, evaluator, result)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Unconfigured),
                        static_cast<int>(result.safety.value().state));

  for (uint64_t sequence = 22U; sequence < 24U; ++sequence) {
    SampleWindow<4U> clear_window{};
    auto clear_sample = ds_sample(sequence, 900U, 4000);
    clear_sample.header.profile_ref = temperature_profile.ref();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                          static_cast<int>(clear_window.try_add(clear_sample)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProcessingSnapshotStatus::Captured),
        static_cast<int>(ProcessingSnapshot::capture(
            1000U, 200U, StreamId::B3, config_ref(), profiles,
            b3_snapshot)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProcessStatus::Produced),
        static_cast<int>(Pipeline::evaluate(clear_window, b3_snapshot, nullptr,
                                            &safety_profile, evaluator,
                                            result)));
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Clear),
                        static_cast<int>(result.safety.value().state));
}

void test_pipeline_maps_valid_mq7_and_mq8_to_b3_safety() {
  ProcessingProfiles profiles{};
  profiles.mq_adc[2] = mq_profile(ProfileTarget::Mq7);
  profiles.mq_adc[3] = mq_profile(ProfileTarget::Mq8);
  profiles.mq_adc_present[2] = true;
  profiles.mq_adc_present[3] = true;
  const auto safety_profile = mq_safety_fixture();

  SampleWindow<4U> window{};
  auto mq7 = mq_sample(30U, SensorId::Mq7, 2400.0F);
  mq7.header.profile_ref = profiles.mq_adc[2].ref();
  auto mq8 = mq_sample(31U, SensorId::Mq8, 2300.0F);
  mq8.header.profile_ref = profiles.mq_adc[3].ref();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(mq7)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(mq8)));
  ProcessingSnapshot snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingSnapshotStatus::Captured),
      static_cast<int>(ProcessingSnapshot::capture(
          1000U, 200U, StreamId::B3, config_ref(), profiles, snapshot)));
  using Pipeline = ProcessingPipeline<4U, 4U>;
  Pipeline::Result result{};
  safety::SafetyEvaluator evaluator{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(Pipeline::evaluate(window, snapshot, nullptr,
                                          &safety_profile, evaluator, result)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Alert),
                        static_cast<int>(result.safety.value().state));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(safety::SafetyReason::HazardThresholdExceeded),
      static_cast<int>(result.safety.value().reason));
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pipeline_preserves_missing_models_without_fabricating_outputs);
  RUN_TEST(test_pipeline_result_can_use_shared_sequence_factory);
  RUN_TEST(test_pipeline_preserves_b3_safety_latch_across_other_streams);
  RUN_TEST(test_pipeline_maps_valid_mq7_and_mq8_to_b3_safety);
  return UNITY_END();
}
