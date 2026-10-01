#include <array>
#include <cstdint>

#include <unity.h>

#include "acquisition/profiles.h"
#include "calibration/calibration_session.h"

namespace {

using namespace sigap::acquisition;
using namespace sigap::calibration;

void test_capture_stats_are_bounded_and_online() {
  CaptureStats stats{};
  stats.observe(10.0);
  stats.observe(14.0);
  stats.observe(12.0);

  TEST_ASSERT_EQUAL_UINT32(3U, stats.count);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 10.0F, static_cast<float>(stats.minimum));
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 14.0F, static_cast<float>(stats.maximum));
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 12.0F, static_cast<float>(stats.mean));
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 4.0F, static_cast<float>(stats.span()));
}

void test_hx711_session_requires_explicit_stability_criteria() {
  CalibrationSession session{};
  const Hx711CaptureCriteria criteria{3U, 5};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::Hx711Organic, criteria, 100U));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Capture),
                        static_cast<int>(session.state()));

  TEST_ASSERT_TRUE(session.observe(Hx711Trial{1000, SampleQuality::Ready}));
  TEST_ASSERT_TRUE(session.observe(Hx711Trial{1002, SampleQuality::Ready}));
  TEST_ASSERT_TRUE(session.observe(Hx711Trial{1004, SampleQuality::Ready}));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Validate),
                        static_cast<int>(session.state()));
  TEST_ASSERT_TRUE(session.validate());
  TEST_ASSERT_TRUE(session.preview());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Preview),
                        static_cast<int>(session.state()));
}

void test_unstable_capture_does_not_preview_or_change_active_profile() {
  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::Hx711Organic,
                                 Hx711CaptureCriteria{3U, 2}, 0U));
  TEST_ASSERT_TRUE(session.observe(Hx711Trial{1000, SampleQuality::Ready}));
  TEST_ASSERT_TRUE(session.observe(Hx711Trial{1004, SampleQuality::Ready}));
  TEST_ASSERT_TRUE(session.observe(Hx711Trial{1005, SampleQuality::Ready}));
  TEST_ASSERT_FALSE(session.validate());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Validate),
                        static_cast<int>(session.state()));
  TEST_ASSERT_FALSE(session.preview());
  TEST_ASSERT_FALSE(session.commit_requested());
}

void test_cancel_returns_idle_without_activating_draft() {
  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::UltrasonicOrganic,
                                 DistanceCaptureCriteria{2U, 1.0F}, 0U));
  TEST_ASSERT_TRUE(session.cancel());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Idle),
                        static_cast<int>(session.state()));
  TEST_ASSERT_FALSE(session.commit_requested());
}

void test_commit_is_external_and_active_profile_is_untouched() {
  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::Mq4,
                                 MqCaptureCriteria{2U, 4U, false}, 0U));
  TEST_ASSERT_TRUE(session.observe(MqTrial{100, SampleQuality::Ready, true}));
  TEST_ASSERT_TRUE(session.observe(MqTrial{102, SampleQuality::Ready, true}));
  TEST_ASSERT_TRUE(session.validate());
  TEST_ASSERT_TRUE(session.preview());
  TEST_ASSERT_TRUE(session.request_commit());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Commit),
                        static_cast<int>(session.state()));
  TEST_ASSERT_TRUE(session.commit_requested());
  TEST_ASSERT_TRUE(session.complete_commit(true));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Idle),
                        static_cast<int>(session.state()));
}

void test_cancel_after_failed_session_returns_idle() {
  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::Ds18b20B3,
                                 Ds18b20CaptureCriteria{1U, 10, true}, 0U));
  session.fail(CalibrationFailure::SensorDisconnected);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Failed),
                        static_cast<int>(session.state()));
  TEST_ASSERT_TRUE(session.cancel());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Idle),
                        static_cast<int>(session.state()));
}

void test_protocol_trials_require_actual_evidence() {
  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::Ds18b20B3,
                                 Ds18b20CaptureCriteria{1U, 20, true}, 0U));
  TEST_ASSERT_FALSE(session.observe(Ds18b20Trial{8500, SampleQuality::Ready,
                                                   false, true, true}));
  TEST_ASSERT_EQUAL_UINT32(1U, session.stats().quality_failures);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Capture),
                        static_cast<int>(session.state()));

  TEST_ASSERT_TRUE(session.observe(Ds18b20Trial{8500, SampleQuality::Ready,
                                                  true, true, true, true}));
  TEST_ASSERT_TRUE(session.validate());
}

void test_profile_validation_helpers_do_not_invent_calibration() {
  Hx711Profile hx{};
  hx.manifest.validity = ProfileValidity::Valid;
  hx.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Hx711Organic);
  hx.counts_per_kg = -500.0F;
  TEST_ASSERT_TRUE(validate_hx711_profile(hx).valid());

  UltrasonicProfile distance{};
  distance.manifest.validity = ProfileValidity::Valid;
  distance.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::UltrasonicOrganic);
  distance.d_empty_mm = 100.0F;
  distance.d_full_mm = 20.0F;
  distance.echo_mm_per_us = 0.1F;
  distance.echo_timeout_us = 30000U;
  distance.inter_sensor_gap_us = 60000U;
  TEST_ASSERT_TRUE(validate_ultrasonic_profile(distance).valid());

  MqAdcProfile mq{};
  mq.manifest.validity = ProfileValidity::Valid;
  mq.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Mq4);
  mq.reference_mv = 3300U;
  mq.full_scale_counts = 4095U;
  mq.divider_numerator = 2U;
  mq.divider_denominator = 1U;
  TEST_ASSERT_TRUE(validate_mq_profile(mq, SensorId::Mq4).valid());
  TEST_ASSERT_FALSE(validate_mq_profile(mq, SensorId::Mq135).valid());

  Ds18b20Profile ds{};
  ds.manifest.validity = ProfileValidity::Valid;
  ds.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  ds.has_rom = true;
  ds.rom = {0x28U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x9eU};
  TEST_ASSERT_TRUE(validate_ds18b20_profile(ds).valid());
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_capture_stats_are_bounded_and_online);
  RUN_TEST(test_hx711_session_requires_explicit_stability_criteria);
  RUN_TEST(test_unstable_capture_does_not_preview_or_change_active_profile);
  RUN_TEST(test_cancel_returns_idle_without_activating_draft);
  RUN_TEST(test_commit_is_external_and_active_profile_is_untouched);
  RUN_TEST(test_cancel_after_failed_session_returns_idle);
  RUN_TEST(test_protocol_trials_require_actual_evidence);
  RUN_TEST(test_profile_validation_helpers_do_not_invent_calibration);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
