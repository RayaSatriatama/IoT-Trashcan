#include <array>
#include <cstdint>

#include <unity.h>

#include "acquisition/record.h"
#include "acquisition/sensors/ds18b20.h"
#include "acquisition/sensors/ultrasonic.h"

namespace {

using namespace sigap::acquisition;

RecordContext context() {
  RecordContext result{};
  result.config_ref.present = true;
  result.config_ref.schema_version = 1U;
  result.config_ref.config_version = 1U;
  result.boot_id.present = true;
  return result;
}

UltrasonicProfile ultrasonic_profile(uint32_t timeout_us,
                                     uint32_t gap_us) {
  UltrasonicProfile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::UltrasonicOrganic);
  profile.d_empty_mm = 100.0F;
  profile.d_full_mm = 20.0F;
  profile.echo_mm_per_us = 0.1F;
  profile.echo_timeout_us = timeout_us;
  profile.inter_sensor_gap_us = gap_us;
  return profile;
}

struct TimedUltrasonicReader final : IUltrasonicReader {
  uint64_t ready_at_us{0U};
  uint32_t trigger_count{0U};

  explicit TimedUltrasonicReader(uint64_t ready_at) noexcept
      : ready_at_us(ready_at) {}

  bool trigger(uint64_t) noexcept override {
    ++trigger_count;
    return true;
  }

  UltrasonicEchoResult poll(uint64_t now_us) noexcept override {
    return now_us >= ready_at_us
               ? UltrasonicEchoResult{UltrasonicEchoStatus::Ready, 500U}
               : UltrasonicEchoResult{UltrasonicEchoStatus::Pending, 0U};
  }
};

void test_ultrasonic_timeout_is_profile_driven() {
  SequenceAllocator first_sequence{};
  RecordFactory first_factory(first_sequence, context());
  TimedUltrasonicReader first_reader{20000U};
  UltrasonicProfile fast = ultrasonic_profile(10000U, 60000U);
  UltrasonicAcquirer first(StreamId::Organic, first_reader, first_factory,
                           &fast, resolve_ultrasonic_params(&fast));
  SampleRecord first_record{};
  first.request_sample(0U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(first.service(20000U, first_record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Timeout),
                        static_cast<int>(first_record.quality));

  SequenceAllocator second_sequence{};
  RecordFactory second_factory(second_sequence, context());
  TimedUltrasonicReader second_reader{20000U};
  UltrasonicProfile slow = ultrasonic_profile(30000U, 60000U);
  UltrasonicAcquirer second(StreamId::Organic, second_reader, second_factory,
                            &slow, resolve_ultrasonic_params(&slow));
  SampleRecord second_record{};
  second.request_sample(0U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(second.service(20000U, second_record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Ready),
                        static_cast<int>(second_record.quality));
}

void test_ultrasonic_effective_gap_uses_conservative_profile_maximum() {
  UltrasonicProfile organic = ultrasonic_profile(30000U, 60000U);
  UltrasonicProfile inorganic = ultrasonic_profile(30000U, 80000U);
  inorganic.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::UltrasonicInorganic);
  UltrasonicProfile b3 = ultrasonic_profile(30000U, 70000U);
  b3.manifest.target_id = static_cast<uint32_t>(ProfileTarget::UltrasonicB3);

  const std::array<const UltrasonicProfile*, 3> profiles{
      &organic, &inorganic, &b3};
  TEST_ASSERT_EQUAL_UINT32(80000U, effective_ultrasonic_gap(profiles));
}

void test_ultrasonic_engineering_fallback_has_no_profile_provenance() {
  UltrasonicProfile invalid_profile = ultrasonic_profile(0U, 0U);
  TEST_ASSERT_FALSE(invalid_profile.valid_for_acquisition());
  const auto params = resolve_ultrasonic_params(nullptr);
  const auto invalid_params = resolve_ultrasonic_params(&invalid_profile);
  TEST_ASSERT_EQUAL_UINT32(
      UltrasonicEngineeringDefaults::kEchoTimeoutUs,
      params.echo_timeout_us);
  TEST_ASSERT_EQUAL_UINT32(
      UltrasonicEngineeringDefaults::kInterSensorGapUs,
      params.inter_sensor_gap_us);
  TEST_ASSERT_FALSE(params.profile_validated);
  TEST_ASSERT_EQUAL_UINT32(params.echo_timeout_us,
                           invalid_params.echo_timeout_us);
  TEST_ASSERT_FALSE(invalid_params.profile_validated);

  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  TimedUltrasonicReader reader{1U};
  UltrasonicAcquirer acquirer(StreamId::Organic, reader, factory,
                              &invalid_profile,
                              params);
  SampleRecord record{};
  acquirer.request_sample(0U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.service(1U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.header.profile_ref.present);
}

struct DsReader final : IDs18b20Reader {
  bool request_ok{true};
  bool ready{true};
  Ds18b20ReadResult result{true, true, true, 8500};

  bool request_conversion() noexcept override { return request_ok; }
  bool conversion_ready() noexcept override { return ready; }
  Ds18b20ReadResult read_conversion() noexcept override { return result; }
};

Ds18b20Profile ds_profile(uint8_t resolution_bits) {
  Ds18b20Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  profile.has_rom = true;
  profile.rom[0] = 0x28U;
  profile.resolution_bits = resolution_bits;
  return profile;
}

void test_ds18b20_resolution_controls_conversion_deadline() {
  TEST_ASSERT_EQUAL_UINT32(94000U, ds18b20_conversion_time_us(9U));
  TEST_ASSERT_EQUAL_UINT32(188000U, ds18b20_conversion_time_us(10U));
  TEST_ASSERT_EQUAL_UINT32(375000U, ds18b20_conversion_time_us(11U));
  TEST_ASSERT_EQUAL_UINT32(750000U, ds18b20_conversion_time_us(12U));
  TEST_ASSERT_EQUAL_UINT32(0U, ds18b20_conversion_time_us(8U));
  TEST_ASSERT_EQUAL_UINT32(0U, ds18b20_conversion_time_us(13U));

  Ds18b20Profile nine_bit = ds_profile(9U);
  Ds18b20Profile twelve_bit = ds_profile(12U);
  TEST_ASSERT_TRUE(nine_bit.valid_for_acquisition());
  TEST_ASSERT_TRUE(twelve_bit.valid_for_acquisition());
  TEST_ASSERT_FALSE(ds_profile(8U).valid_for_acquisition());
  TEST_ASSERT_FALSE(ds_profile(13U).valid_for_acquisition());
  TEST_ASSERT_EQUAL_UINT32(94000U,
                           resolve_ds18b20_conversion_time(&nine_bit));
  TEST_ASSERT_EQUAL_UINT32(750000U,
                           resolve_ds18b20_conversion_time(&twelve_bit));
}

void test_ds18b20_incomplete_conversion_is_not_ready() {
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  DsReader source{};
  source.ready = false;
  Ds18b20Profile profile = ds_profile(9U);
  Ds18b20Acquirer acquirer(source, factory, &profile,
                           ds18b20_conversion_time_us(profile.resolution_bits));
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(94000U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Timeout),
                        static_cast<int>(record.quality));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::ConversionPending),
                        static_cast<int>(record.reason));
  TEST_ASSERT_FALSE(record.converted.primary_available);

  source.ready = true;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(100000U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(194000U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Ready),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 85.0F,
                           record.converted.primary_value);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ultrasonic_timeout_is_profile_driven);
  RUN_TEST(test_ultrasonic_effective_gap_uses_conservative_profile_maximum);
  RUN_TEST(test_ultrasonic_engineering_fallback_has_no_profile_provenance);
  RUN_TEST(test_ds18b20_resolution_controls_conversion_deadline);
  RUN_TEST(test_ds18b20_incomplete_conversion_is_not_ready);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
