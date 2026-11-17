#include <unity.h>
#include "calibration/calibration_session.h"
#include "acquisition/profile_store.h"
using namespace sigap::acquisition;
using namespace sigap::calibration;
void setUp() {}
void tearDown() {}
static Mq135GasCaptureCriteria criteria() {
  Mq135GasCaptureCriteria c{};
  c.required_samples = 3; c.reference_confirmed = true;
  c.profile.electrical = {5000, 10000, 10000, 15000, Mq135Topology::DirectLoaded, true};
  c.profile.criteria = {3, 2000, 500, 10};
  c.profile.hardware_identity[0] = 'S'; c.profile.reference_environment[0] = 'S';
  c.profile.temperature_centi_c = 2500; c.profile.humidity_centi_percent = 5000;
  c.profile.heating_duration_us = 10000; c.profile.preparation_confirmed = true;
  auto& a = c.adc_profile;
  a.manifest.kind = ProfileKind::Adc; a.manifest.target_id = 8;
  a.manifest.profile_version = 1; a.manifest.validity = ProfileValidity::TestOnly;
  a.manifest.provenance.present = true; a.manifest.provenance.evidence_count = 3;
  a.reference_mv = 3000; a.full_scale_counts = 3000;
  a.divider_numerator = 3; a.divider_denominator = 5;
  ProfileCodec codec{}; EncodedProfile bytes{};
  TEST_ASSERT_TRUE(codec.encode(a, bytes)); TEST_ASSERT_TRUE(codec.decode(bytes, a));
  return c;
}
static MqTrial trial(const Mq135GasCaptureCriteria& c, uint64_t seq, uint64_t at) {
  MqTrial t{}; t.adc_counts = 1500; t.quality = SampleQuality::Ready;
  t.observed_at_us = at; t.source_sequence = seq; t.source_profile = c.adc_profile.ref();
  t.source_boot_id.present = true; t.source_boot_id.bytes.fill(1);
  return t;
}
void test_gas_uses_the_existing_session_and_device_evidence() {
  const auto c = criteria(); CalibrationSession s{};
  TEST_ASSERT_TRUE(s.begin(CalibrationTarget::Mq135Gas, 1000));
  TEST_ASSERT_TRUE(s.configure(c, 1000));
  for (uint64_t i = 1; i <= 3; ++i) {
    s.touch(i * 1000);
    TEST_ASSERT_TRUE(s.observe(trial(c, i, i * 1000)));
  }
  TEST_ASSERT_EQUAL(CalibrationState::Validate, s.state());
  TEST_ASSERT_TRUE(s.gas_summary().ready);
  TEST_ASSERT_FLOAT_WITHIN(.01, 50000.0/7.0, s.gas_summary().median_ohm);
  TEST_ASSERT_TRUE(s.validate()); TEST_ASSERT_TRUE(s.preview());
}
void test_missing_reference_or_unmeasured_adc_cannot_begin_capture() {
  auto c = criteria(); CalibrationSession s{};
  TEST_ASSERT_TRUE(s.begin(CalibrationTarget::Mq135Gas, 1000));
  c.reference_confirmed = false; TEST_ASSERT_FALSE(s.configure(c, 1000));
  c = criteria(); c.adc_profile.manifest.provenance.present = false;
  TEST_ASSERT_FALSE(s.configure(c, 1000));
  c = criteria(); c.profile.preparation_confirmed = false;
  TEST_ASSERT_FALSE(s.configure(c, 1000));
}
void test_stale_and_changed_profile_sources_poison_reference() {
  auto c = criteria(); CalibrationSession s{};
  TEST_ASSERT_TRUE(s.start(CalibrationTarget::Mq135Gas, c, 1000));
  auto t = trial(c, 1, 1000); s.touch(2000);
  TEST_ASSERT_FALSE(s.observe(t));
  t = trial(c, 2, 2000); t.source_profile.hash[0] ^= 1;
  TEST_ASSERT_FALSE(s.observe(t));
  TEST_ASSERT_FALSE(s.gas_summary().ready);
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gas_uses_the_existing_session_and_device_evidence);
  RUN_TEST(test_missing_reference_or_unmeasured_adc_cannot_begin_capture);
  RUN_TEST(test_stale_and_changed_profile_sources_poison_reference);
  return UNITY_END();
}
