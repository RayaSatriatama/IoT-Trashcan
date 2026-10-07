#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

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

ProfileManifest profile_manifest(ProfileKind kind, ProfileTarget target) {
  ProfileManifest manifest{};
  manifest.kind = kind;
  manifest.schema_version = 1U;
  manifest.profile_version = 1U;
  manifest.target_id = static_cast<uint32_t>(target);
  manifest.validity = ProfileValidity::Valid;
  manifest.hash[0] = 0x31U;
  return manifest;
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

void test_processing_cadence_is_independent_of_summary_period() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  TEST_ASSERT_EQUAL_UINT64(1'000'000U, runtime.processing_period_us());
}

void test_runtime_retains_latest_attempt_per_channel_with_bounded_budget() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  const std::array<std::pair<SensorId, StreamId>, 13U> channels{{
      {SensorId::Hx711Organic, StreamId::Organic},
      {SensorId::Hx711Inorganic, StreamId::Inorganic},
      {SensorId::Hx711B3, StreamId::B3},
      {SensorId::UltrasonicOrganic, StreamId::Organic},
      {SensorId::UltrasonicInorganic, StreamId::Inorganic},
      {SensorId::UltrasonicB3, StreamId::B3},
      {SensorId::Mq4, StreamId::Organic},
      {SensorId::Mq135, StreamId::Organic},
      {SensorId::Mq7, StreamId::B3},
      {SensorId::Mq8, StreamId::B3},
      {SensorId::Ds18b20B3, StreamId::B3},
      {SensorId::Sht31_44, StreamId::Organic},
      {SensorId::Sht31_45, StreamId::B3},
  }};
  uint64_t sequence_number = 100U;
  for (uint32_t round = 0U; round < 20U; ++round) {
    for (const auto [sensor, stream] : channels) {
      auto record = organic_attempt(sequence_number++,
                                    100'000U + round * 100'000U);
      record.header.sensor = sensor;
      record.header.stream = stream;
      runtime.observe(record);
    }
  }

  TEST_ASSERT_EQUAL_UINT32(13U, runtime.retained_samples());
  TEST_ASSERT_EQUAL_UINT64(0U, runtime.dropped_samples());
  TEST_ASSERT_EQUAL_UINT64(260U, runtime.observed_samples());
}

void test_late_poll_uses_observed_time_not_stale_deadline() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  runtime.observe(organic_attempt(500U, 900'000U));
  runtime.poll(2'500'000U);

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_EQUAL_UINT64(2'500'000U, result.header.monotonic_us);
}

void test_capacity_profile_is_captured_in_runtime_snapshot() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  Hx711Profile hx{};
  hx.manifest = profile_manifest(ProfileKind::Calibration,
                                  ProfileTarget::Hx711Organic);
  hx.counts_per_kg = 500.0F;
  hx.tare_offset_counts = 1000;
  profiles.hx711[0] = &hx;

  processing::SlotCapacityProfile capacity{};
  capacity.present = true;
  capacity.stream = StreamId::Organic;
  capacity.operational_capacity_kg = 4.0F;
  capacity.manifest.kind = ProfileKind::Capacity;
  capacity.manifest.schema_version = 1U;
  capacity.manifest.profile_version = 2U;
  capacity.manifest.target_id = 1U;
  capacity.manifest.validity = ProfileValidity::TestOnly;
  capacity.manifest.hash[0] = 0x42U;
  profiles.capacity[0] = &capacity;

  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);
  auto record = organic_attempt(700U, 900'000U);
  record.header.profile_ref = hx.ref();
  record.quality = SampleQuality::Ready;
  record.reason = SampleReason::DataReady;
  record.raw.present = true;
  record.raw.signed_counts = 2000;
  runtime.observe(record);
  runtime.poll(1'000'000U);

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_TRUE(result.result.processing.processed.weight_utilization_pct.available);
  TEST_ASSERT_FLOAT_WITHIN(
      0.001F, 50.0F,
      result.result.processing.processed.weight_utilization_pct.value);
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

SampleRecord sht31_attempt(uint64_t sequence, uint64_t monotonic_us,
                            const Sht31Profile& profile) {
  auto record = organic_attempt(sequence, monotonic_us);
  record.header.sensor = SensorId::Sht31_44;
  record.header.kind = SampleKind::Sht31;
  record.header.profile_ref = profile.ref();
  record.quality = SampleQuality::Ready;
  record.reason = SampleReason::DataReady;
  record.raw.present = true;
  record.raw.identity_valid = true;
  record.raw.temperature_crc_valid = true;
  record.raw.temperature_centi_c = 2500;
  return record;
}

void test_runtime_preserves_slow_channel_until_freshness_expires() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  Sht31Profile sht{};
  sht.manifest = profile_manifest(ProfileKind::Calibration, ProfileTarget::Sht31_44);
  sht.address = 0x44U;
  profiles.sht31[0] = &sht;
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);
  runtime.observe(sht31_attempt(42U, 900'000U, sht));

  for (const uint64_t now_us : {1'000'000ULL, 2'000'000ULL}) {
    runtime.poll(now_us);
    ProcessingRuntime::ResultEnvelope result{};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(processing::ResultHandoffStatus::Dequeued),
        static_cast<int>(runtime.handoff().try_pop(result)));
    TEST_ASSERT_TRUE(result.result.processing.processed.temperature_c.available);
    TEST_ASSERT_FLOAT_WITHIN(0.001F, 25.0F,
                             result.result.processing.processed.temperature_c.value);
    TEST_ASSERT_EQUAL_UINT64(42U,
        result.result.processing.source_sample_ids.at(0U)->sequence);
    TEST_ASSERT_EQUAL_UINT64(now_us, result.header.monotonic_us);
  }

  runtime.poll(6'000'000U);
  ProcessingRuntime::ResultEnvelope stale{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(stale)));
  TEST_ASSERT_FALSE(stale.result.processing.processed.temperature_c.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::InputQuality::Stale),
      static_cast<int>(stale.result.processing.processed.temperature_c.disposition.quality));
  TEST_ASSERT_EQUAL_UINT64(42U,
      stale.result.processing.source_sample_ids.at(0U)->sequence);
  TEST_ASSERT_EQUAL_UINT32(1U, downstream.count);
}

void test_runtime_newest_failed_attempt_supersedes_retained_ready() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  Sht31Profile sht{};
  sht.manifest = profile_manifest(ProfileKind::Calibration, ProfileTarget::Sht31_44);
  sht.address = 0x44U;
  profiles.sht31[0] = &sht;
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);
  runtime.observe(sht31_attempt(42U, 900'000U, sht));
  runtime.poll(1'000'000U);
  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_TRUE(result.result.processing.processed.temperature_c.available);

  auto failed = sht31_attempt(43U, 1'900'000U, sht);
  failed.quality = SampleQuality::Timeout;
  failed.reason = SampleReason::Timeout;
  failed.raw.present = false;
  runtime.observe(failed);
  runtime.poll(2'000'000U);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_FALSE(result.result.processing.processed.temperature_c.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::InputQuality::Timeout),
      static_cast<int>(result.result.processing.processed.temperature_c.disposition.quality));
  TEST_ASSERT_EQUAL_UINT64(43U,
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
  RUN_TEST(test_processing_cadence_is_independent_of_summary_period);
  RUN_TEST(test_runtime_retains_latest_attempt_per_channel_with_bounded_budget);
  RUN_TEST(test_late_poll_uses_observed_time_not_stale_deadline);
  RUN_TEST(test_capacity_profile_is_captured_in_runtime_snapshot);
  RUN_TEST(test_runtime_cycle_emits_unconfigured_result_with_shared_provenance);
  RUN_TEST(test_runtime_full_result_handoff_is_observable_and_preserves_old_result);
  RUN_TEST(test_runtime_preserves_slow_channel_until_freshness_expires);
  RUN_TEST(test_runtime_newest_failed_attempt_supersedes_retained_ready);
  RUN_TEST(test_processing_resource_footprint_is_bounded);
  return UNITY_END();
}
