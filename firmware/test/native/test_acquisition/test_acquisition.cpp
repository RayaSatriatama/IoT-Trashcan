#include <array>
#include <cstdint>

#include <unity.h>

#include "acquisition/acquisition_types.h"
#include "acquisition/handoff.h"
#include "acquisition/heater.h"
#include "acquisition/health.h"
#include "acquisition/profiles.h"
#include "acquisition/record.h"
#include "acquisition/runtime.h"
#include "acquisition/scheduler.h"
#include "acquisition/sequence.h"
#include "acquisition/sensors/ds18b20.h"
#include "acquisition/sensors/hx711.h"
#include "acquisition/sensors/mq_adc.h"
#include "acquisition/sensors/sht31.h"
#include "acquisition/sensors/ultrasonic.h"

namespace {

using namespace sigap::acquisition;

constexpr ConfigRef kConfigRef{
    1U,
    7U,
    sigap::config::ConfigHash{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U,
                              11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U,
                              19U, 20U, 21U, 22U, 23U, 24U, 25U, 26U,
                              27U, 28U, 29U, 30U, 31U, 32U},
    true};

BootId boot_id() {
  BootId id{};
  id.present = true;
  id.bytes[0] = 0xa5U;
  id.bytes[15] = 0x5aU;
  return id;
}

RecordContext record_context() {
  return RecordContext{boot_id(), DeviceId{}, kConfigRef, nullptr};
}

void test_sequence_is_global_and_non_wrapping() {
  SequenceAllocator allocator;
  uint64_t first{0};
  uint64_t second{0};
  TEST_ASSERT_TRUE(allocator.allocate(first));
  TEST_ASSERT_TRUE(allocator.allocate(second));
  TEST_ASSERT_EQUAL_UINT64(0, first);
  TEST_ASSERT_EQUAL_UINT64(1, second);
  TEST_ASSERT_EQUAL_UINT64(2, allocator.next_value());

  SequenceAllocator exhausted(UINT64_MAX);
  uint64_t value{0};
  TEST_ASSERT_FALSE(exhausted.allocate(value));
  TEST_ASSERT_TRUE(exhausted.exhausted());
}

void test_record_preserves_time_quality_and_config_provenance() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  SampleRecord record{};
  RawObservation raw{};
  raw.present = false;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(DriverPollStatus::Produced),
      static_cast<int>(factory.make(
          1234U, SensorId::Mq4, StreamId::Organic, SampleKind::MqAdc,
          SampleQuality::Timeout, SampleReason::Timeout, raw,
          ConvertedObservation{}, ProfileRef{}, record)));
  TEST_ASSERT_EQUAL_UINT64(0, record.header.sequence);
  TEST_ASSERT_EQUAL_UINT64(1234U, record.header.monotonic_us);
  TEST_ASSERT_FALSE(record.header.recorded_at.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TimeQuality::Unsynced),
                        static_cast<int>(record.header.time_quality));
  TEST_ASSERT_EQUAL_UINT64(kConfigRef.config_version,
                           record.header.config_ref.config_version);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kConfigRef.hash.data(),
                                record.header.config_ref.hash.data(),
                                kConfigRef.hash.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Timeout),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.raw.present);
}

struct JobProbe {
  uint32_t calls{0};
  uint64_t last_now{0};
  static void run(void* context, uint64_t now_us) noexcept {
    auto* probe = static_cast<JobProbe*>(context);
    ++probe->calls;
    probe->last_now = now_us;
  }
};

void test_scheduler_is_monotonic_and_failure_isolated() {
  CooperativeScheduler scheduler;
  JobProbe first{};
  JobProbe second{};
  TEST_ASSERT_TRUE(scheduler.add_job(100U, &JobProbe::run, &first, 0U));
  TEST_ASSERT_TRUE(scheduler.add_job(250U, &JobProbe::run, &second, 0U));
  scheduler.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1, first.calls);
  TEST_ASSERT_EQUAL_UINT32(1, second.calls);
  scheduler.poll(99U);
  TEST_ASSERT_EQUAL_UINT32(1, first.calls);
  scheduler.poll(100U);
  TEST_ASSERT_EQUAL_UINT32(2, first.calls);
  TEST_ASSERT_EQUAL_UINT32(100, first.last_now);
  scheduler.poll(250U);
  TEST_ASSERT_EQUAL_UINT32(2, second.calls);
  TEST_ASSERT_EQUAL_UINT32(250, second.last_now);
}

void test_bounded_handoff_reports_full_without_overwrite() {
  SampleRing<2> queue;
  SampleRecord first{};
  SampleRecord second{};
  SampleRecord third{};
  first.header.sequence = 1;
  second.header.sequence = 2;
  third.header.sequence = 3;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(queue.try_push(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(queue.try_push(second)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Full),
                        static_cast<int>(queue.try_push(third)));
  TEST_ASSERT_EQUAL_UINT64(1, queue.dropped_records());
  SampleRecord out{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                        static_cast<int>(queue.try_pop(out)));
  TEST_ASSERT_EQUAL_UINT64(1, out.header.sequence);
}

void test_profile_validity_gates_conversion() {
  Hx711Profile hx{};
  hx.manifest.validity = ProfileValidity::Valid;
  hx.counts_per_kg = 1000.0F;
  TEST_ASSERT_TRUE(hx.valid_for_conversion());
  hx.counts_per_kg = 0.0F;
  TEST_ASSERT_FALSE(hx.valid_for_conversion());

  UltrasonicProfile distance{};
  distance.manifest.validity = ProfileValidity::Valid;
  distance.d_empty_mm = 1000.0F;
  distance.d_full_mm = 200.0F;
  distance.echo_mm_per_us = 0.1F;
  TEST_ASSERT_TRUE(distance.valid_for_conversion());
  distance.d_full_mm = 1000.0F;
  TEST_ASSERT_FALSE(distance.valid_for_conversion());
}

struct HeaterRailFake final : IMq7HeaterRail {
  uint32_t high_calls{0};
  uint32_t low_calls{0};
  bool select_high() noexcept override { ++high_calls; return true; }
  bool select_low() noexcept override { ++low_calls; return true; }
};

void test_mq7_heater_uses_profile_defined_phases() {
  constexpr Mq7HeaterProfile fixture{
      ProfileRef{}, 10U, 20U, 30U};
  HeaterRailFake rail{};
  Mq7HeaterController heater(rail, fixture);
  heater.start(100U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::Preheat),
                        static_cast<int>(heater.state()));
  heater.tick(109U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::Preheat),
                        static_cast<int>(heater.state()));
  heater.tick(110U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::HighClean),
                        static_cast<int>(heater.state()));
  heater.tick(130U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::LowDetect),
                        static_cast<int>(heater.state()));
  TEST_ASSERT_TRUE(heater.operational_ready());
  heater.reset(200U);
  TEST_ASSERT_FALSE(heater.operational_ready());
}

struct HxFake final : IHx711Reader {
  bool ready_value{true};
  Hx711ReadResult result{true, false, 12000, 1, 1, SampleReason::DataReady};
  bool ready() noexcept override { return ready_value; }
  Hx711ReadResult read() noexcept override { return result; }
};

void test_hx711_retains_raw_and_requires_calibration() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  HxFake source{};
  Hx711Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Hx711Organic);
  profile.counts_per_kg = 1000.0F;
  Hx711Acquirer acquirer(SensorId::Hx711Organic, StreamId::Organic, source,
                         factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(10U, record)));
  TEST_ASSERT_TRUE(record.raw.present);
  TEST_ASSERT_EQUAL_INT64(12000, record.raw.signed_counts);
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 12.0F, record.converted.primary_value);

  Hx711Acquirer uncalibrated(SensorId::Hx711Organic, StreamId::Organic, source,
                             factory, nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(uncalibrated.poll(20U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

struct UltrasonicFake final : IUltrasonicReader {
  bool triggered{false};
  UltrasonicEchoResult result{UltrasonicEchoStatus::Pending, 0U};
  bool trigger(uint64_t) noexcept override { triggered = true; return true; }
  UltrasonicEchoResult poll(uint64_t) noexcept override { return result; }
};

void test_ultrasonic_is_nonblocking_and_rejects_invalid_profile() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  UltrasonicFake source{};
  UltrasonicProfile invalid{};
  invalid.manifest.validity = ProfileValidity::Valid;
  invalid.manifest.target_id = static_cast<uint32_t>(ProfileTarget::UltrasonicOrganic);
  invalid.d_empty_mm = 100.0F;
  invalid.d_full_mm = 100.0F;
  UltrasonicAcquirer acquirer(StreamId::Organic, source, factory, &invalid,
                              30000U);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(0U, record)));
  source.result = {UltrasonicEchoStatus::Ready, 1000U};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(1U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                        static_cast<int>(record.quality));
}

struct MqFake final : IMqAdcReader {
  MqAdcReadResult result{true, false, 2048U, SampleReason::DataReady};
  MqAdcReadResult read() noexcept override { return result; }
};

void test_mq_adc_keeps_raw_without_inventing_ppm() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  MqFake source{};
  MqAdcProfile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Mq4);
  profile.reference_mv = 3300U;
  profile.full_scale_counts = 4096U;
  profile.divider_numerator = 2U;
  profile.divider_denominator = 3U;
  MqAdcAcquirer acquirer(SensorId::Mq4, StreamId::Organic, source, factory,
                         &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_TRUE(record.raw.present);
  TEST_ASSERT_EQUAL_UINT16(2048U, record.raw.adc_counts);
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConvertedKind::AdcMillivolts),
                        static_cast<int>(record.converted.primary_kind));
  TEST_ASSERT_FALSE(record.gas_ppm_available);
}

void test_mq7_invalid_heater_phase_keeps_diagnostic_adc_only() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  MqFake source{};
  MqAdcProfile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Mq7);
  profile.reference_mv = 3300U;
  profile.full_scale_counts = 4096U;
  profile.divider_numerator = 1U;
  profile.divider_denominator = 1U;
  constexpr Mq7HeaterProfile heater_profile{ProfileRef{}, 10U, 20U, 30U};
  HeaterRailFake rail{};
  Mq7HeaterController heater(rail, heater_profile);
  heater.start(0U);
  MqAdcAcquirer acquirer(SensorId::Mq7, StreamId::B3, source, factory,
                         &profile, &heater);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::HeaterPhase),
                        static_cast<int>(record.quality));
  TEST_ASSERT_TRUE(record.raw.present);
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

struct DsFake final : IDs18b20Reader {
  bool request_ok{true};
  Ds18b20ReadResult result{};
  bool request_conversion() noexcept override { return request_ok; }
  Ds18b20ReadResult read_conversion() noexcept override { return result; }
};

void test_ds18b20_conversion_is_split_and_verified_85c_is_valid() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  DsFake source{};
  source.result.present = true;
  source.result.crc_valid = true;
  source.result.conversion_complete = true;
  source.result.temperature_centi_c = 8500;
  Ds18b20Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  profile.has_rom = true;
  profile.rom[0] = 0x28U;
  Ds18b20Acquirer acquirer(source, factory, &profile, 750U);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(749U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(750U, record)));
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 85.0F, record.converted.primary_value);
}

struct ShtFake final : ISht31Reader {
  Sht31ReadResult result{};
  Sht31ReadResult read(uint8_t) noexcept override { return result; }
};

void test_ultrasonic_trigger_gate_enforces_shared_gap() {
  UltrasonicTriggerGate gate;
  TEST_ASSERT_TRUE(gate.can_trigger(0U));
  gate.note_trigger(0U);
  TEST_ASSERT_FALSE(gate.can_trigger(59999U));
  TEST_ASSERT_TRUE(gate.can_trigger(60000U));
}

void test_hx711_not_ready_produces_typed_attempt_without_raw_evidence() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  HxFake source{};
  source.ready_value = false;
  Hx711Acquirer acquirer(SensorId::Hx711Organic, StreamId::Organic, source,
                         factory, nullptr);
  SampleRecord record{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(3U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Timeout),
                        static_cast<int>(record.quality));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::NotReady),
                        static_cast<int>(record.reason));
  TEST_ASSERT_FALSE(record.raw.present);
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

void test_sensor_failures_are_typed_and_raw_evidence_is_not_fabricated() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  HxFake source{};
  source.result = {false, true, -55, 1, 1, SampleReason::Disconnected};
  Hx711Acquirer acquirer(SensorId::Hx711Organic, StreamId::Organic, source,
                         factory, nullptr);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(2U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Disconnected),
                        static_cast<int>(record.quality));
  TEST_ASSERT_EQUAL_INT64(-55, record.raw.signed_counts);
  TEST_ASSERT_FALSE(record.raw.present);
  TEST_ASSERT_FALSE(record.converted.primary_available);

  MqFake mq_source{};
  mq_source.result = {false, true, 321U, SampleReason::Disconnected};
  MqAdcAcquirer mq_acquirer(SensorId::Mq4, StreamId::Organic, mq_source,
                            factory, nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(mq_acquirer.poll(3U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Disconnected),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.raw.present);
  TEST_ASSERT_EQUAL_UINT16(321U, record.raw.adc_counts);
}

void test_ds18b20_crc_failure_is_not_a_temperature() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  DsFake source{};
  source.result = {true, false, true, 8500};
  Ds18b20Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  profile.has_rom = true;
  profile.rom[0] = 0x28U;
  Ds18b20Acquirer acquirer(source, factory, &profile, 1U);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(1U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::CrcError),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

void test_ds18b20_missing_device_produces_disconnected_attempt() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  DsFake source{};
  source.request_ok = false;
  Ds18b20Acquirer acquirer(source, factory, nullptr, 750U);
  SampleRecord record{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Disconnected),
                        static_cast<int>(record.quality));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::Disconnected),
                        static_cast<int>(record.reason));
  TEST_ASSERT_FALSE(record.raw.present);
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

void test_sht31_crc_failure_preserves_failure_reason() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  ShtFake source{};
  source.result = {true, false, 2500, 6000};
  Sht31Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Sht31_45);
  profile.address = 0x45U;
  Sht31Acquirer acquirer(0x45U, StreamId::Ambient, source, factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::CrcError),
                        static_cast<int>(record.quality));
  TEST_ASSERT_TRUE(record.raw.present);
  TEST_ASSERT_EQUAL_INT32(6000, record.raw.humidity_centi_pct);
  TEST_ASSERT_FALSE(record.converted.secondary_available);
}

void test_acquisition_memory_footprint_is_bounded() {
  UnityPrint("memory acquisition SampleRecord=");
  UnityPrintNumber(static_cast<int>(sizeof(SampleRecord)));
  UnityPrint(" SequenceAllocator=");
  UnityPrintNumber(static_cast<int>(sizeof(SequenceAllocator)));
  UnityPrint(" SampleRing16=");
  UnityPrintNumber(static_cast<int>(sizeof(SampleRing<16>)));
  UnityPrint(" HealthState=");
  UnityPrintNumber(static_cast<int>(sizeof(SensorHealthState)));
  UnityPrint(" Scheduler=");
  UnityPrintNumber(static_cast<int>(sizeof(CooperativeScheduler)));
  UnityPrint(" AcquisitionFoundation=");
  UnityPrintNumber(static_cast<int>(sizeof(AcquisitionFoundation)));
  UnityPrint(" TargetGraph=");
  UnityPrintNumber(static_cast<int>(sizeof(TargetAcquisitionGraph)));
  UnityPrint(" TargetAcquisition=");
  UnityPrintNumber(static_cast<int>(sizeof(TargetAcquisition)));
  UnityPrint(" Hx711Profile=");
  UnityPrintNumber(static_cast<int>(sizeof(Hx711Profile)));
  UnityPrint(" UltrasonicProfile=");
  UnityPrintNumber(static_cast<int>(sizeof(UltrasonicProfile)));
  UnityPrint(" MqAdcProfile=");
  UnityPrintNumber(static_cast<int>(sizeof(MqAdcProfile)));
  UnityPrint(" Mq7HeaterProfile=");
  UnityPrintNumber(static_cast<int>(sizeof(Mq7HeaterProfile)));
  UnityPrint("\\n");
  TEST_ASSERT_LESS_OR_EQUAL_UINT(512U, sizeof(SampleRecord));
  TEST_ASSERT_LESS_OR_EQUAL_UINT(16U * sizeof(SampleRecord) + 64U,
                                  sizeof(SampleRing<16>));
}

void test_sht31_keeps_temperature_and_humidity_independent() {
  SequenceAllocator allocator;
  RecordFactory factory(allocator, record_context());
  ShtFake source{};
  source.result.ok = true;
  source.result.crc_valid = true;
  source.result.temperature_centi_c = 2500;
  source.result.humidity_centi_pct = 6000;
  Sht31Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Sht31_44);
  profile.address = 0x44U;
  Sht31Acquirer acquirer(0x44U, StreamId::Organic, source, factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_TRUE(record.converted.secondary_available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 25.0F, record.converted.primary_value);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 60.0F, record.converted.secondary_value);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_sequence_is_global_and_non_wrapping);
  RUN_TEST(test_record_preserves_time_quality_and_config_provenance);
  RUN_TEST(test_scheduler_is_monotonic_and_failure_isolated);
  RUN_TEST(test_bounded_handoff_reports_full_without_overwrite);
  RUN_TEST(test_profile_validity_gates_conversion);
  RUN_TEST(test_mq7_heater_uses_profile_defined_phases);
  RUN_TEST(test_hx711_retains_raw_and_requires_calibration);
  RUN_TEST(test_ultrasonic_is_nonblocking_and_rejects_invalid_profile);
  RUN_TEST(test_mq_adc_keeps_raw_without_inventing_ppm);
  RUN_TEST(test_mq7_invalid_heater_phase_keeps_diagnostic_adc_only);
  RUN_TEST(test_ds18b20_conversion_is_split_and_verified_85c_is_valid);
  RUN_TEST(test_sht31_keeps_temperature_and_humidity_independent);
  RUN_TEST(test_ultrasonic_trigger_gate_enforces_shared_gap);
  RUN_TEST(test_hx711_not_ready_produces_typed_attempt_without_raw_evidence);
  RUN_TEST(test_sensor_failures_are_typed_and_raw_evidence_is_not_fabricated);
  RUN_TEST(test_ds18b20_crc_failure_is_not_a_temperature);
  RUN_TEST(test_ds18b20_missing_device_produces_disconnected_attempt);
  RUN_TEST(test_sht31_crc_failure_preserves_failure_reason);
  RUN_TEST(test_acquisition_memory_footprint_is_bounded);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
