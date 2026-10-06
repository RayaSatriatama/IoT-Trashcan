#include <array>
#include <cstdint>
#include <variant>

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
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Capture),
                        static_cast<int>(session.state()));
  TEST_ASSERT_TRUE(session.observe_anchor(Hx711Trial{1500, SampleQuality::Ready},
                                          CalibrationAnchor::Reference));
  TEST_ASSERT_TRUE(session.observe_anchor(Hx711Trial{1502, SampleQuality::Ready},
                                          CalibrationAnchor::Reference));
  TEST_ASSERT_TRUE(session.observe_anchor(Hx711Trial{1504, SampleQuality::Ready},
                                          CalibrationAnchor::Reference));
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
  TEST_ASSERT_TRUE(session.observe_anchor(Hx711Trial{1200, SampleQuality::Ready},
                                          CalibrationAnchor::Reference));
  TEST_ASSERT_TRUE(session.observe_anchor(Hx711Trial{1204, SampleQuality::Ready},
                                          CalibrationAnchor::Reference));
  TEST_ASSERT_TRUE(session.observe_anchor(Hx711Trial{1205, SampleQuality::Ready},
                                          CalibrationAnchor::Reference));
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

void test_calibrating_quality_accepts_only_physical_evidence() {
  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::Mq4,
                                 MqCaptureCriteria{1U, 10U, false}, 0U));
  TEST_ASSERT_FALSE(session.observe(MqTrial{100U, SampleQuality::Calibrating,
                                            true, false}));
  TEST_ASSERT_TRUE(session.observe(MqTrial{100U, SampleQuality::Calibrating,
                                           true, true}));
  TEST_ASSERT_EQUAL_UINT32(1U, session.stats().count);
}

void test_evidence_cache_maps_ds18b20_physical_flags_without_drift() {
  CalibrationEvidenceCache cache{};
  SampleRecord sample{};
  sample.header.sensor = SensorId::Ds18b20B3;
  sample.header.kind = SampleKind::Ds18b20;
  sample.header.sequence = 1U;
  sample.quality = SampleQuality::Calibrating;
  sample.raw.present = true;
  sample.raw.identity_valid = true;
  sample.raw.conversion_complete = true;
  sample.raw.crc_valid = true;
  sample.raw.temperature_centi_c = 2350;
  cache.observe(sample);

  TrialObservation observation{};
  uint64_t sequence{0U};
  TEST_ASSERT_TRUE(cache.latest(CalibrationTarget::Ds18b20B3, 0U,
                               observation, sequence));
  const auto& trial = std::get<Ds18b20Trial>(observation);
  TEST_ASSERT_TRUE(trial.raw_present);
  TEST_ASSERT_TRUE(trial.rom_match);
  TEST_ASSERT_TRUE(trial.conversion_complete);
  TEST_ASSERT_TRUE(trial.crc_valid);

  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.start(CalibrationTarget::Ds18b20B3,
                                 Ds18b20CaptureCriteria{1U, 10, true}, 0U));
  TEST_ASSERT_TRUE(session.observe(observation));
  TEST_ASSERT_TRUE(session.validate());

  sample.header.sequence = 2U;
  sample.raw.present = false;
  cache.observe(sample);
  TEST_ASSERT_TRUE(cache.latest(CalibrationTarget::Ds18b20B3, 1U,
                                observation, sequence));
  TEST_ASSERT_FALSE(std::get<Ds18b20Trial>(observation).raw_present);
}

void test_evidence_cache_keeps_sht31_addresses_and_crc_fields_independent() {
  CalibrationEvidenceCache cache{};
  SampleRecord organic{};
  organic.header.sensor = SensorId::Sht31_44;
  organic.header.kind = SampleKind::Sht31;
  organic.header.sequence = 1U;
  organic.quality = SampleQuality::Calibrating;
  organic.raw.present = true;
  organic.raw.temperature_crc_valid = true;
  organic.raw.humidity_crc_valid = false;
  cache.observe(organic);

  SampleRecord b3 = organic;
  b3.header.sensor = SensorId::Sht31_45;
  b3.header.sequence = 2U;
  b3.raw.temperature_crc_valid = false;
  b3.raw.humidity_crc_valid = true;
  cache.observe(b3);

  TrialObservation observation{};
  uint64_t sequence{0U};
  TEST_ASSERT_TRUE(cache.latest(CalibrationTarget::Sht31_44, 0U,
                               observation, sequence));
  const auto& organic_trial = std::get<Sht31Trial>(observation);
  TEST_ASSERT_TRUE(organic_trial.temperature_crc_valid);
  TEST_ASSERT_FALSE(organic_trial.humidity_crc_valid);
  TEST_ASSERT_TRUE(cache.latest(CalibrationTarget::Sht31_45, 0U,
                               observation, sequence));
  const auto& b3_trial = std::get<Sht31Trial>(observation);
  TEST_ASSERT_FALSE(b3_trial.temperature_crc_valid);
  TEST_ASSERT_TRUE(b3_trial.humidity_crc_valid);
}

void test_evidence_cache_rejects_each_ds18b20_physical_failure() {
  for (uint8_t failure = 0U; failure < 4U; ++failure) {
    CalibrationEvidenceCache cache{};
    SampleRecord sample{};
    sample.header.sensor = SensorId::Ds18b20B3;
    sample.header.kind = SampleKind::Ds18b20;
    sample.header.sequence = 1U;
    sample.quality = SampleQuality::Calibrating;
    sample.raw.present = true;
    sample.raw.identity_valid = true;
    sample.raw.conversion_complete = true;
    sample.raw.crc_valid = true;
    if (failure == 0U) sample.raw.present = false;
    if (failure == 1U) sample.raw.identity_valid = false;
    if (failure == 2U) sample.raw.conversion_complete = false;
    if (failure == 3U) sample.raw.crc_valid = false;
    cache.observe(sample);

    TrialObservation observation{};
    uint64_t sequence{0U};
    TEST_ASSERT_TRUE(cache.latest(CalibrationTarget::Ds18b20B3, 0U,
                                 observation, sequence));
    CalibrationSession session{};
    TEST_ASSERT_TRUE(session.start(CalibrationTarget::Ds18b20B3,
                                   Ds18b20CaptureCriteria{1U, 10, true}, 0U));
    TEST_ASSERT_FALSE(session.observe(observation));
  }
}

void test_configuration_phase_binds_target_and_expires_without_criteria() {
  CalibrationSession session{};
  TEST_ASSERT_TRUE(session.begin(CalibrationTarget::Ds18b20B3, 100U));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Configuring),
                        static_cast<int>(session.state()));
  TEST_ASSERT_FALSE(session.start(CalibrationTarget::Hx711Organic,
                                  Hx711CaptureCriteria{1U, 1}, 100U));
  TEST_ASSERT_TRUE(session.tick(100U + CalibrationSession::kSessionTimeoutUs + 1U));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CalibrationState::Failed),
                        static_cast<int>(session.state()));
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
  RUN_TEST(test_calibrating_quality_accepts_only_physical_evidence);
  RUN_TEST(test_evidence_cache_maps_ds18b20_physical_flags_without_drift);
  RUN_TEST(test_evidence_cache_keeps_sht31_addresses_and_crc_fields_independent);
  RUN_TEST(test_evidence_cache_rejects_each_ds18b20_physical_failure);
  RUN_TEST(test_configuration_phase_binds_target_and_expires_without_criteria);
  RUN_TEST(test_profile_validation_helpers_do_not_invent_calibration);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
