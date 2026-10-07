#include <cmath>
#include <cstdint>

#include <unity.h>

#include "processing/processing_engine.h"

namespace {

using namespace sigap::acquisition;
using namespace sigap::processing;

ConfigRef config_ref(uint64_t version, uint8_t seed = 1U) {
  ConfigRef ref{};
  ref.schema_version = 1U;
  ref.config_version = version;
  ref.present = true;
  for (std::size_t index = 0U; index < ref.hash.size(); ++index) {
    ref.hash[index] = static_cast<uint8_t>(seed + index);
  }
  return ref;
}

ProfileManifest manifest(ProfileKind kind, ProfileTarget target,
                         uint32_t version, uint8_t seed = 1U) {
  ProfileManifest value{};
  value.kind = kind;
  value.schema_version = 1U;
  value.profile_version = version;
  value.target_id = static_cast<uint32_t>(target);
  value.validity = ProfileValidity::Valid;
  for (std::size_t index = 0U; index < value.hash.size(); ++index) {
    value.hash[index] = static_cast<uint8_t>(seed + index);
  }
  return value;
}

DeviceId device_id() {
  DeviceId id{};
  id.present = true;
  id.bytes[0] = 0x51U;
  return id;
}

BootId boot_id() {
  BootId id{};
  id.present = true;
  id.bytes[0] = 0xa1U;
  return id;
}

SampleRecord sample(SensorId sensor, StreamId stream, uint64_t sequence,
                    uint64_t monotonic_us, SampleQuality quality,
                    const ProfileRef& profile) {
  SampleRecord value{};
  value.header.sensor = sensor;
  value.header.stream = stream;
  value.header.device_id = device_id();
  value.header.boot_id = boot_id();
  value.header.sequence = sequence;
  value.header.monotonic_us = monotonic_us;
  value.header.config_ref = config_ref(4U);
  value.header.profile_ref = profile;
  value.quality = quality;
  value.reason = quality == SampleQuality::Ready
                     ? SampleReason::DataReady
                     : SampleReason::InvalidProfile;
  return value;
}

ProcessingProfiles organic_profiles() {
  ProcessingProfiles profiles{};
  profiles.hx711_present = true;
  profiles.hx711.manifest =
      manifest(ProfileKind::Calibration, ProfileTarget::Hx711Organic, 3U,
               0x10U);
  profiles.hx711.tare_offset_counts = 1000;
  profiles.hx711.counts_per_kg = 500.0F;

  profiles.ultrasonic_present = true;
  profiles.ultrasonic.manifest = manifest(
      ProfileKind::Calibration, ProfileTarget::UltrasonicOrganic, 5U, 0x30U);
  profiles.ultrasonic.d_empty_mm = 1000.0F;
  profiles.ultrasonic.d_full_mm = 200.0F;
  profiles.ultrasonic.echo_mm_per_us = 0.1F;
  profiles.ultrasonic.overshoot_tolerance_mm = 10.0F;
  profiles.ultrasonic.echo_timeout_us = 30000U;
  profiles.ultrasonic.inter_sensor_gap_us = 60000U;

  profiles.capacity.present = true;
  profiles.capacity.stream = StreamId::Organic;
  profiles.capacity.operational_capacity_kg = 4.0F;
  profiles.capacity.ref.kind = ProfileKind::Capacity;
  profiles.capacity.ref.schema_version = 1U;
  profiles.capacity.ref.profile_version = 2U;
  profiles.capacity.ref.target_id = 1U;
  profiles.capacity.ref.present = true;
  profiles.capacity.ref.hash[0] = 0xc1U;
  return profiles;
}

void test_window_rejects_duplicate_and_full_without_overwriting() {
  SampleWindow<2U> window{};
  const auto first = sample(SensorId::Hx711Organic, StreamId::Organic, 1U,
                            100U, SampleQuality::Ready, ProfileRef{});
  const auto second = sample(SensorId::Hx711Organic, StreamId::Organic, 2U,
                             200U, SampleQuality::Ready, ProfileRef{});
  const auto third = sample(SensorId::Hx711Organic, StreamId::Organic, 3U,
                            300U, SampleQuality::Ready, ProfileRef{});

  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Duplicate),
                        static_cast<int>(window.try_add(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(second)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Full),
                        static_cast<int>(window.try_add(third)));
  TEST_ASSERT_EQUAL_UINT32(2U, window.size());
  TEST_ASSERT_TRUE(window.overflowed());
}

void test_valid_samples_are_converted_normalized_and_provenanced() {
  SampleWindow<8U> window{};
  auto profiles = organic_profiles();
  const auto hx_ref = profiles.hx711.ref();
  const auto ultrasonic_ref = profiles.ultrasonic.ref();

  auto hx_first = sample(SensorId::Hx711Organic, StreamId::Organic, 10U,
                         900000U, SampleQuality::Ready, hx_ref);
  hx_first.raw.present = true;
  hx_first.raw.signed_counts = 1500;
  auto hx_second = sample(SensorId::Hx711Organic, StreamId::Organic, 11U,
                          950000U, SampleQuality::Ready, hx_ref);
  hx_second.raw.present = true;
  hx_second.raw.signed_counts = 2500;
  auto distance = sample(SensorId::UltrasonicOrganic, StreamId::Organic, 12U,
                         975000U, SampleQuality::Ready, ultrasonic_ref);
  distance.raw.present = true;
  distance.raw.distance_us = 5000U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(hx_first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(hx_second)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowStatus::Added),
                        static_cast<int>(window.try_add(distance)));

  ProcessingSnapshot snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingSnapshotStatus::Captured),
      static_cast<int>(ProcessingSnapshot::capture(
          1000000U, 200000U, StreamId::Organic, config_ref(4U), profiles,
          snapshot)));

  ProcessedResult<8U> result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(SlotProcessor<8U, 8U>::process(window, snapshot,
                                                       result)));
  TEST_ASSERT_TRUE(result.processed.weight_kg.available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 2.0F,
                           result.processed.weight_kg.value);
  TEST_ASSERT_TRUE(result.processed.weight_utilization_pct.available);
  TEST_ASSERT_FLOAT_WITHIN(
      0.001F, 50.0F, result.processed.weight_utilization_pct.value);
  TEST_ASSERT_TRUE(result.processed.fill_pct.available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 62.5F,
                           result.processed.fill_pct.value);
  TEST_ASSERT_FALSE(result.processed.ogi.available);
  TEST_ASSERT_FALSE(result.processed.accumulation_hours.available);
  TEST_ASSERT_EQUAL_UINT32(3U, result.source_sample_ids.size());
  TEST_ASSERT_EQUAL_UINT32(3U, result.profile_refs.size());
  TEST_ASSERT_EQUAL_UINT64(4U, result.config_ref.config_version);
}

void test_stale_and_calibrating_samples_remain_unavailable() {
  SampleWindow<4U> window{};
  auto profiles = organic_profiles();
  auto stale = sample(SensorId::Hx711Organic, StreamId::Organic, 20U, 100U,
                      SampleQuality::Ready, profiles.hx711.ref());
  stale.raw.present = true;
  stale.raw.signed_counts = 1500;
  auto calibrating = sample(SensorId::UltrasonicOrganic, StreamId::Organic,
                            21U, 950U, SampleQuality::Calibrating,
                            profiles.ultrasonic.ref());
  calibrating.raw.present = true;
  calibrating.raw.distance_us = 5000U;
  window.try_add(stale);
  window.try_add(calibrating);

  ProcessingSnapshot snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingSnapshotStatus::Captured),
      static_cast<int>(ProcessingSnapshot::capture(
          1000U, 100U, StreamId::Organic, config_ref(4U), profiles,
          snapshot)));
  ProcessedResult<4U> result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(SlotProcessor<4U, 4U>::process(window, snapshot,
                                                       result)));
  TEST_ASSERT_FALSE(result.processed.weight_kg.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(InputQuality::Stale),
                        static_cast<int>(result.processed.weight_kg.disposition.quality));
  TEST_ASSERT_FALSE(result.processed.fill_pct.available);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(InputQuality::Calibrating),
      static_cast<int>(result.processed.fill_pct.disposition.quality));
  TEST_ASSERT_EQUAL_UINT32(2U, result.source_sample_ids.size());
}

void test_latest_hx711_attempt_blocks_older_ready_average() {
  const std::array<SampleQuality, 3U> blocked_qualities{
      SampleQuality::Calibrating, SampleQuality::Timeout,
      SampleQuality::Disconnected};

  for (const auto blocked_quality : blocked_qualities) {
    SampleWindow<4U> window{};
    const auto profiles = organic_profiles();
    auto ready = sample(SensorId::Hx711Organic, StreamId::Organic, 60U,
                        900U, SampleQuality::Ready, profiles.hx711.ref());
    ready.raw.present = true;
    ready.raw.signed_counts = 2000;
    auto blocked = sample(SensorId::Hx711Organic, StreamId::Organic, 61U,
                          950U, blocked_quality, profiles.hx711.ref());
    window.try_add(ready);
    window.try_add(blocked);

    ProcessingSnapshot snapshot{};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProcessingSnapshotStatus::Captured),
        static_cast<int>(ProcessingSnapshot::capture(
            1000U, 200U, StreamId::Organic, config_ref(4U), profiles,
            snapshot)));
    ProcessedResult<4U> result{};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProcessStatus::Produced),
        static_cast<int>(SlotProcessor<4U, 4U>::process(window, snapshot,
                                                         result)));
    TEST_ASSERT_FALSE(result.processed.weight_kg.available);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(sample_disposition(blocked_quality).quality),
        static_cast<int>(result.processed.weight_kg.disposition.quality));
  }
}

void test_profile_mismatch_does_not_reinterpret_raw_evidence() {
  SampleWindow<2U> window{};
  auto profiles = organic_profiles();
  auto wrong_ref = profiles.hx711.ref();
  ++wrong_ref.profile_version;
  auto value = sample(SensorId::Hx711Organic, StreamId::Organic, 30U, 900U,
                      SampleQuality::Ready, wrong_ref);
  value.raw.present = true;
  value.raw.signed_counts = 1500;
  window.try_add(value);

  ProcessingSnapshot snapshot{};
  ProcessingSnapshot::capture(1000U, 200U, StreamId::Organic,
                              config_ref(4U), profiles, snapshot);
  ProcessedResult<2U> result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(SlotProcessor<2U, 2U>::process(window, snapshot,
                                                       result)));
  TEST_ASSERT_FALSE(result.processed.weight_kg.available);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingReason::ProfileInvalid),
      static_cast<int>(result.processed.weight_kg.disposition.reason));
}

void test_conflicting_profile_identity_is_not_silently_deduplicated() {
  ProfileRefSet refs{};
  const auto first = organic_profiles().hx711.ref();
  auto conflicting = first;
  ++conflicting.profile_version;
  conflicting.hash[0] ^= 0x5aU;

  TEST_ASSERT_TRUE(add_profile(first, refs));
  TEST_ASSERT_FALSE(add_profile(conflicting, refs));
  TEST_ASSERT_EQUAL_UINT32(1U, refs.size());
  TEST_ASSERT_EQUAL_UINT32(first.profile_version,
                           refs.at(0U)->profile_version);
}

void test_source_capacity_overflow_rejects_whole_result() {
  SampleWindow<3U> window{};
  auto profiles = organic_profiles();
  for (uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    auto value = sample(SensorId::Hx711Organic, StreamId::Organic, sequence,
                        900U + sequence, SampleQuality::Ready,
                        profiles.hx711.ref());
    value.raw.present = true;
    value.raw.signed_counts = 1500;
    window.try_add(value);
  }
  ProcessingSnapshot snapshot{};
  ProcessingSnapshot::capture(1000U, 200U, StreamId::Organic,
                              config_ref(4U), profiles, snapshot);
  ProcessedResult<2U> result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::SourceCapacityExceeded),
      static_cast<int>(SlotProcessor<3U, 2U>::process(window, snapshot,
                                                       result)));
  TEST_ASSERT_EQUAL_UINT32(0U, result.source_sample_ids.size());
}

void test_cycle_snapshot_is_immutable_after_profiles_change() {
  auto profiles = organic_profiles();
  ProcessingSnapshot snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingSnapshotStatus::Captured),
      static_cast<int>(ProcessingSnapshot::capture(
          1000U, 200U, StreamId::Organic, config_ref(4U), profiles,
          snapshot)));
  profiles.hx711.counts_per_kg = 1000.0F;
  profiles.hx711.manifest.profile_version = 99U;
  profiles.capacity.operational_capacity_kg = 20.0F;

  TEST_ASSERT_FLOAT_WITHIN(0.001F, 500.0F,
                           snapshot.profiles().hx711.counts_per_kg);
  TEST_ASSERT_EQUAL_UINT32(
      3U, snapshot.profiles().hx711.manifest.profile_version);
  TEST_ASSERT_FLOAT_WITHIN(
      0.001F, 4.0F,
      snapshot.profiles().capacity.operational_capacity_kg);
}

void test_invalid_capacity_suppresses_only_utilization() {
  SampleWindow<2U> window{};
  auto profiles = organic_profiles();
  profiles.capacity.operational_capacity_kg = 0.0F;
  auto value = sample(SensorId::Hx711Organic, StreamId::Organic, 40U, 900U,
                      SampleQuality::Ready, profiles.hx711.ref());
  value.raw.present = true;
  value.raw.signed_counts = 1500;
  window.try_add(value);
  ProcessingSnapshot snapshot{};
  ProcessingSnapshot::capture(1000U, 200U, StreamId::Organic,
                              config_ref(4U), profiles, snapshot);
  ProcessedResult<2U> result{};
  SlotProcessor<2U, 2U>::process(window, snapshot, result);

  TEST_ASSERT_TRUE(result.processed.weight_kg.available);
  TEST_ASSERT_FALSE(result.processed.weight_utilization_pct.available);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingReason::CapacityInvalid),
      static_cast<int>(result.processed.weight_utilization_pct.disposition.reason));
}

void test_sht31_temperature_uses_crc_identity_and_profile_offset() {
  SampleWindow<2U> window{};
  ProcessingProfiles profiles{};
  profiles.temperature_source = TemperatureSource::Sht31_44;
  profiles.sht31_present = true;
  profiles.sht31.manifest = manifest(
      ProfileKind::Calibration, ProfileTarget::Sht31_44, 6U, 0x55U);
  profiles.sht31.address = 0x44U;
  profiles.sht31.temperature_offset_centi_c = -25;

  auto value = sample(SensorId::Sht31_44, StreamId::Organic, 50U, 900U,
                      SampleQuality::Ready, profiles.sht31.ref());
  value.raw.present = true;
  value.raw.identity_valid = true;
  value.raw.temperature_crc_valid = true;
  value.raw.temperature_centi_c = 2525;
  window.try_add(value);

  ProcessingSnapshot snapshot{};
  ProcessingSnapshot::capture(1000U, 200U, StreamId::Organic,
                              config_ref(4U), profiles, snapshot);
  ProcessedResult<2U> result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(SlotProcessor<2U, 2U>::process(window, snapshot,
                                                       result)));
  TEST_ASSERT_TRUE(result.processed.temperature_c.available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 25.0F,
                           result.processed.temperature_c.value);

  value.header.sequence = 51U;
  value.raw.temperature_crc_valid = false;
  SampleWindow<2U> invalid_window{};
  invalid_window.try_add(value);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessStatus::Produced),
      static_cast<int>(SlotProcessor<2U, 2U>::process(invalid_window,
                                                       snapshot, result)));
  TEST_ASSERT_FALSE(result.processed.temperature_c.available);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ProcessingReason::ConversionFailed),
      static_cast<int>(result.processed.temperature_c.disposition.reason));
}

struct FuzzyFixture final {
  uint8_t state{0U};
  float score{0.0F};
};

struct SafetyFixture final {
  uint8_t state{0U};
};

void test_result_handoff_is_bounded_and_keeps_fuzzy_safety_distinct() {
  using Envelope = EvaluatedResult<FuzzyFixture, SafetyFixture, 2U>;
  EvaluatedResultRing<Envelope, 1U> ring{};
  Envelope first{ProcessedResult<2U>{}, FuzzyFixture{1U, 32.56F},
                 SafetyFixture{2U}};
  Envelope second{ProcessedResult<2U>{}, FuzzyFixture{2U, 70.0F},
                  SafetyFixture{3U}};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ResultHandoffStatus::Enqueued),
                        static_cast<int>(ring.try_push(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ResultHandoffStatus::Full),
                        static_cast<int>(ring.try_push(second)));
  TEST_ASSERT_EQUAL_UINT64(1U, ring.dropped_results());

  Envelope output{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ResultHandoffStatus::Dequeued),
                        static_cast<int>(ring.try_pop(output)));
  TEST_ASSERT_EQUAL_UINT8(1U, output.fuzzy.value().state);
  TEST_ASSERT_EQUAL_UINT8(2U, output.safety.value().state);
}

void test_result_factory_consumes_shared_sequence_and_preserves_provenance() {
  SequenceAllocator shared_sequence{};
  uint64_t sample_sequence{UINT64_MAX};
  TEST_ASSERT_TRUE(shared_sequence.allocate(sample_sequence));
  TEST_ASSERT_EQUAL_UINT64(0U, sample_sequence);

  ResultRecordContext context{};
  context.device_id = device_id();
  context.boot_id = boot_id();
  context.config_ref = config_ref(4U, 0x21U);
  ResultRecordFactory factory(shared_sequence, context);

  ProcessedResult<2U> processed{};
  processed.stream = StreamId::Organic;
  processed.computed_monotonic_us = 990U;
  processed.config_ref = context.config_ref;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(processed.source_sample_ids.try_add(
          SourceSampleId{device_id(), boot_id(), sample_sequence})));
  EvaluatedResult<FuzzyFixture, SafetyFixture, 2U> evaluated{
      processed, FuzzyFixture{1U, 32.56F}, SafetyFixture{2U}};

  ResultEnvelope<FuzzyFixture, SafetyFixture, 2U> envelope{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ResultFactoryStatus::Produced),
      static_cast<int>(factory.make(1000U, evaluated, envelope)));
  TEST_ASSERT_EQUAL_UINT64(1U, envelope.header.sequence);
  TEST_ASSERT_EQUAL_UINT64(1000U, envelope.header.monotonic_us);
  TEST_ASSERT_TRUE(envelope.header.device_id.present);
  TEST_ASSERT_TRUE(envelope.header.boot_id.present);
  TEST_ASSERT_FALSE(envelope.header.recorded_at.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::Unsynced),
                        static_cast<int>(envelope.header.time_quality));
  TEST_ASSERT_EQUAL_UINT64(4U,
                           envelope.header.config_ref.config_version);
  TEST_ASSERT_EQUAL_UINT64(990U,
                           envelope.result.processing.computed_monotonic_us);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_window_rejects_duplicate_and_full_without_overwriting);
  RUN_TEST(test_valid_samples_are_converted_normalized_and_provenanced);
  RUN_TEST(test_stale_and_calibrating_samples_remain_unavailable);
  RUN_TEST(test_latest_hx711_attempt_blocks_older_ready_average);
  RUN_TEST(test_profile_mismatch_does_not_reinterpret_raw_evidence);
  RUN_TEST(test_conflicting_profile_identity_is_not_silently_deduplicated);
  RUN_TEST(test_source_capacity_overflow_rejects_whole_result);
  RUN_TEST(test_cycle_snapshot_is_immutable_after_profiles_change);
  RUN_TEST(test_invalid_capacity_suppresses_only_utilization);
  RUN_TEST(test_sht31_temperature_uses_crc_identity_and_profile_offset);
  RUN_TEST(test_result_handoff_is_bounded_and_keeps_fuzzy_safety_distinct);
  RUN_TEST(
      test_result_factory_consumes_shared_sequence_and_preserves_provenance);
  return UNITY_END();
}
