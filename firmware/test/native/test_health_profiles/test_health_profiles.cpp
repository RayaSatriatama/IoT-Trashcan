#include <cmath>
#include <cstdint>

#include <unity.h>

#include "acquisition/health.h"
#include "acquisition/heater.h"
#include "acquisition/profiles.h"
#include "acquisition/record.h"
#include "acquisition/sensors/hx711.h"
#include "acquisition/sensors/sht31.h"
#include "acquisition/sensors/ultrasonic.h"

namespace {

using namespace sigap::acquisition;

ConfigRef config_ref() {
  ConfigRef ref{};
  ref.present = true;
  ref.schema_version = 1U;
  ref.config_version = 3U;
  return ref;
}

RecordContext context() {
  RecordContext result{};
  result.config_ref = config_ref();
  result.boot_id.present = true;
  return result;
}

void test_health_readiness_does_not_fake_hardware_failure() {
  SensorHealthState state{};
  for (uint32_t index = 0; index < 100U; ++index) {
    state.observe(index, SampleQuality::WarmingUp);
    state.observe(index, SampleQuality::HeaterPhase);
    state.observe(index, SampleQuality::Uncalibrated);
    state.observe(index, SampleQuality::Calibrating);
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorHealth::Online),
                        static_cast<int>(state.health));
  TEST_ASSERT_EQUAL_UINT32(0U, state.consecutive_failures);

  state.observe(1000U, SampleQuality::Timeout);
  state.observe(1001U, SampleQuality::Timeout);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorHealth::Degraded),
                        static_cast<int>(state.health));
  state.observe(1002U, SampleQuality::Timeout);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorHealth::Failed),
                        static_cast<int>(state.health));
  state.observe(1003U, SampleQuality::Ready);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorHealth::Online),
                        static_cast<int>(state.health));
  TEST_ASSERT_EQUAL_UINT32(0U, state.consecutive_failures);
}

struct HeaterRailFake final : IMq7HeaterRail {
  bool fail_high{false};
  bool fail_low{false};
  uint32_t high_calls{0};
  uint32_t low_calls{0};

  bool select_high() noexcept override {
    ++high_calls;
    return !fail_high;
  }

  bool select_low() noexcept override {
    ++low_calls;
    return !fail_low;
  }
};

void test_mq7_heater_profile_cycles_until_reset() {
  constexpr Mq7HeaterProfile profile{ProfileRef{}, 10U, 20U, 30U};
  HeaterRailFake rail{};
  Mq7HeaterController controller(rail, profile);
  controller.start(0U);
  controller.tick(10U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::HighClean),
                        static_cast<int>(controller.state()));
  controller.tick(30U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::LowDetect),
                        static_cast<int>(controller.state()));
  TEST_ASSERT_TRUE(controller.operational_ready());
  controller.tick(60U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::HighClean),
                        static_cast<int>(controller.state()));
  TEST_ASSERT_FALSE(controller.operational_ready());
  controller.tick(80U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::LowDetect),
                        static_cast<int>(controller.state()));
  TEST_ASSERT_TRUE(controller.operational_ready());

  controller.reset(100U);
  TEST_ASSERT_FALSE(controller.operational_ready());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::Preheat),
                        static_cast<int>(controller.state()));
}

void test_mq7_heater_rejects_unknown_profile_and_rail_fault() {
  constexpr Mq7HeaterProfile invalid{ProfileRef{}, 0U, 20U, 30U};
  HeaterRailFake rail{};
  Mq7HeaterController invalid_controller(rail, invalid);
  invalid_controller.start(0U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::Fault),
                        static_cast<int>(invalid_controller.state()));
  TEST_ASSERT_EQUAL_UINT32(0U, rail.high_calls);

  constexpr Mq7HeaterProfile profile{ProfileRef{}, 1U, 1U, 1U};
  rail.fail_high = true;
  Mq7HeaterController failed_controller(rail, profile);
  failed_controller.start(0U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq7HeaterState::Fault),
                        static_cast<int>(failed_controller.state()));
}

struct HxReaderFake final : IHx711Reader {
  Hx711ReadResult result{true, false, 1500, 1, 1, SampleReason::DataReady};
  bool ready() noexcept override { return true; }
  Hx711ReadResult read() noexcept override { return result; }
};

void test_hx711_tare_and_signed_calibration_factor() {
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  HxReaderFake reader{};
  Hx711Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::Hx711Organic);
  profile.tare_offset_counts = 1000;
  profile.counts_per_kg = 500.0F;
  Hx711Acquirer acquirer(SensorId::Hx711Organic, StreamId::Organic, reader,
                         factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(1U, record)));
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 1.0F, record.converted.primary_value);

  profile.counts_per_kg = -500.0F;
  reader.result.counts = 500;
  TEST_ASSERT_TRUE(profile.valid_for_conversion());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(2U, record)));
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 1.0F, record.converted.primary_value);

  profile.counts_per_kg = 0.0F;
  TEST_ASSERT_FALSE(profile.valid_for_conversion());
  profile.counts_per_kg = NAN;
  TEST_ASSERT_FALSE(profile.valid_for_conversion());
  profile.counts_per_kg = INFINITY;
  TEST_ASSERT_FALSE(profile.valid_for_conversion());
}

void test_profile_target_binding_rejects_wrong_hx711_channel() {
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  HxReaderFake reader{};
  Hx711Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Hx711Organic);
  profile.counts_per_kg = 100.0F;
  Hx711Acquirer wrong_channel(SensorId::Hx711B3, StreamId::B3, reader,
                              factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(wrong_channel.poll(3U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

struct UltrasonicReaderFake final : IUltrasonicReader {
  UltrasonicEchoResult result{UltrasonicEchoStatus::Ready, 1000U};
  bool trigger(uint64_t) noexcept override { return true; }
  UltrasonicEchoResult poll(uint64_t) noexcept override { return result; }
};

void test_ultrasonic_tolerance_clamps_only_small_envelope_overshoot() {
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  UltrasonicReaderFake reader{};
  UltrasonicProfile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::UltrasonicOrganic);
  profile.d_empty_mm = 100.0F;
  profile.d_full_mm = 20.0F;
  profile.echo_mm_per_us = 0.1F;
  profile.overshoot_tolerance_mm = 2.0F;
  UltrasonicAcquirer acquirer(StreamId::Organic, reader, factory, &profile, 30000U);
  SampleRecord record{};

  // 100 mm is the empty endpoint and remains a valid zero-fill sample.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(1U, record)));
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 0.0F, record.converted.primary_value);

  // 21 mm is one millimetre below D_full and inside the profile tolerance.
  reader.result.echo_us = 190U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(60001U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(60002U, record)));
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 100.0F, record.converted.primary_value);

  // 25 mm is outside the allowed envelope for this direction only if below
  // D_full - tolerance.  Use 17 mm to exercise that guard.
  reader.result.echo_us = 170U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.poll(120003U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.poll(120004U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::OutOfRange),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

void test_sht31_profile_address_binding() {
  struct Reader final : ISht31Reader {
    Sht31ReadResult read(uint8_t) noexcept override { return {true, true, 2500, 5000}; }
  } reader;
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  Sht31Profile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Sht31_44);
  profile.address = 0x44U;
  Sht31Acquirer wrong_address(0x45U, StreamId::Ambient, reader, factory, &profile);
  SampleRecord record{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(wrong_address.poll(0U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.converted.primary_available);
  TEST_ASSERT_TRUE(record.raw.present);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_health_readiness_does_not_fake_hardware_failure);
  RUN_TEST(test_mq7_heater_profile_cycles_until_reset);
  RUN_TEST(test_mq7_heater_rejects_unknown_profile_and_rail_fault);
  RUN_TEST(test_hx711_tare_and_signed_calibration_factor);
  RUN_TEST(test_profile_target_binding_rejects_wrong_hx711_channel);
  RUN_TEST(test_ultrasonic_tolerance_clamps_only_small_envelope_overshoot);
  RUN_TEST(test_sht31_profile_address_binding);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
