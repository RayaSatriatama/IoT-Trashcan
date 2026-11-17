#include <cmath>
#include <limits>
#include <unity.h>
#include "acquisition/mq135_gas.h"
#include "acquisition/profile_manager.h"

using namespace sigap::acquisition;

void setUp() {}
void tearDown() {}

static MqAdcProfile adc() {
  MqAdcProfile p{};
  p.manifest.kind = ProfileKind::Adc;
  p.manifest.target_id = 8;
  p.manifest.profile_version = 1;
  p.manifest.validity = ProfileValidity::TestOnly;
  p.reference_mv = 3000; p.full_scale_counts = 3000;
  p.divider_numerator = 3; p.divider_denominator = 5;
  return p;
}
static Mq135Electrical electrical() {
  Mq135Electrical p{};
  p.circuit_mv = 5000; p.module_load_ohm = 10000;
  p.divider_top_ohm = 10000; p.divider_bottom_ohm = 15000;
  p.topology = Mq135Topology::DirectLoaded;
  p.verified = true;
  return p;
}
void test_electrical_independent_reference_and_no_double_divider() {
  const auto r = mq135_resistance(1500, adc(), electrical());
  TEST_ASSERT_TRUE(r.ready());
  TEST_ASSERT_FLOAT_WITHIN(.01, 1500, r.adc_pin_mv);
  TEST_ASSERT_FLOAT_WITHIN(.01, 2500, r.ao_mv);
  // RL = 10000 || 25000 = 50000/7; Rs = RL * (5000/2500 - 1).
  TEST_ASSERT_FLOAT_WITHIN(.01, 50000.0/7.0, r.rs_ohm);
  auto a = adc(); auto e = electrical();
  a.divider_numerator = 2; a.divider_denominator = 5;
  e.divider_top_ohm = 15000; e.divider_bottom_ohm = 10000;
  const auto b = mq135_resistance(1000, a, e);
  TEST_ASSERT_TRUE(b.ready());
  TEST_ASSERT_FLOAT_WITHIN(.01, 2500, b.ao_mv);
  TEST_ASSERT_FLOAT_WITHIN(.01, 50000.0/7.0, b.rs_ohm);
}
void test_invalid_inputs_never_create_values() {
  auto e = electrical(); auto a = adc();
  TEST_ASSERT_FALSE(mq135_resistance(0, a, e).ready());
  TEST_ASSERT_FALSE(mq135_resistance(3000, a, e).ready());
  TEST_ASSERT_FALSE(mq135_resistance(3100, a, e).ready());
  e.verified = false;
  TEST_ASSERT_FALSE(mq135_resistance(1500, a, e).ready());
  e = electrical(); e.module_load_ohm = std::numeric_limits<float>::quiet_NaN();
  TEST_ASSERT_FALSE(mq135_resistance(1500, a, e).ready());
  e = electrical(); e.topology = static_cast<Mq135Topology>(99);
  TEST_ASSERT_FALSE(mq135_resistance(1500, a, e).ready());
  e = electrical(); e.divider_top_ohm = 15000;
  TEST_ASSERT_FALSE(mq135_resistance(1500, a, e).ready());
  e = electrical(); e.circuit_mv = 2000;
  TEST_ASSERT_FALSE(mq135_resistance(1500, a, e).ready());
}
void test_reference_is_bounded_timestamped_median_not_arbitrary_r0() {
  Mq135ReferenceWindow w{};
  Mq135ReferenceCriteria c{};
  c.samples = 3; c.min_duration_us = 200; c.max_age_us = 100;
  c.max_variation_percent = 10;
  TEST_ASSERT_TRUE(w.configure(c));
  TEST_ASSERT_TRUE(w.observe(100, 1, 100, 100));
  TEST_ASSERT_FALSE(w.observe(100, 1, 100, 100)); // duplicate poisons window
  TEST_ASSERT_FALSE(w.summary().ready);
  TEST_ASSERT_TRUE(w.configure(c));
  TEST_ASSERT_TRUE(w.observe(102, 1, 100, 100));
  TEST_ASSERT_TRUE(w.observe(98, 2, 200, 200));
  TEST_ASSERT_TRUE(w.observe(100, 3, 300, 300));
  auto s = w.summary();
  TEST_ASSERT_TRUE(s.ready);
  TEST_ASSERT_FLOAT_WITHIN(.001, 100, s.median_ohm);
  TEST_ASSERT_FLOAT_WITHIN(.001, 4, s.variation_percent);
  TEST_ASSERT_FALSE(w.observe(100, 4, 400, 400));
  c.samples = 65;
  TEST_ASSERT_FALSE(w.configure(c));
}
void test_stale_outlier_short_duration_and_invalid_reference_fail_closed() {
  Mq135ReferenceWindow w{}; Mq135ReferenceCriteria c{3, 200, 100, 10};
  TEST_ASSERT_TRUE(w.configure(c));
  TEST_ASSERT_FALSE(w.observe(100, 1, 100, 201));
  TEST_ASSERT_FALSE(w.summary().ready);
  TEST_ASSERT_TRUE(w.configure(c));
  TEST_ASSERT_TRUE(w.observe(100, 1, 100, 100));
  TEST_ASSERT_TRUE(w.observe(1000, 2, 200, 200));
  TEST_ASSERT_TRUE(w.observe(100, 3, 300, 300));
  TEST_ASSERT_FALSE(w.summary().ready);
  TEST_ASSERT_TRUE(w.configure(c));
  TEST_ASSERT_TRUE(w.observe(100, 1, 100, 100));
  TEST_ASSERT_TRUE(w.observe(100, 2, 101, 101));
  TEST_ASSERT_TRUE(w.observe(100, 3, 102, 102));
  TEST_ASSERT_FALSE(w.summary().ready);
  TEST_ASSERT_FALSE(w.observe(INFINITY, 4, 300, 300));
}
static Mq135GasProfile gas_fixture(uint32_t version = 1) {
  Mq135GasProfile p{};
  p.manifest.kind = ProfileKind::Calibration;
  p.manifest.target_id = 8; p.manifest.profile_version = version;
  p.manifest.validity = ProfileValidity::TestOnly;
  p.manifest.provenance.present = true;
  p.manifest.provenance.evidence_count = 3;
  p.manifest.provenance.evidence_sequence = 3;
  p.electrical = electrical(); p.adc_version = 1;
  EncodedProfile encoded{}; ProfileCodec codec{};
  TEST_ASSERT_TRUE(codec.encode(adc(), encoded)); p.adc_hash = encoded.hash();
  p.criteria = {3, 200, 100, 10};
  Mq135ReferenceWindow w{}; TEST_ASSERT_TRUE(w.configure(p.criteria));
  TEST_ASSERT_TRUE(w.observe(100, 1, 100, 100));
  TEST_ASSERT_TRUE(w.observe(102, 2, 200, 200));
  TEST_ASSERT_TRUE(w.observe(98, 3, 300, 300));
  p.reference = w.summary(); p.r0_ohm = p.reference.median_ohm;
  p.hardware_identity[0] = 'S'; p.reference_environment[0] = 'S';
  p.temperature_centi_c = 2500; p.humidity_centi_percent = 5000;
  p.heating_duration_us = 100; p.preparation_confirmed = true;
  return p;
}
void test_gas_profile_roundtrip_atomic_commit_reboot_rollback_and_adc_isolation() {
  ProfileCodec codec{}; MemoryProfileStore store{}; ProfilePersistence persist(store);
  ProfileManager manager(persist); EncodedProfile a{}, g{}, restored{};
  TargetProfileSet runtime{}; manager.bind_runtime_profiles(runtime);
  TEST_ASSERT_TRUE(codec.encode(adc(), a));
  TEST_ASSERT_TRUE(manager.stage_import(ProfileTarget::Mq135, a));
  TEST_ASSERT_TRUE(manager.preview(ProfileTarget::Mq135));
  TEST_ASSERT_TRUE(manager.commit(ProfileTarget::Mq135));
  auto p = gas_fixture();
  TEST_ASSERT_TRUE(p.valid()); TEST_ASSERT_TRUE(codec.encode(p, g));
  TEST_ASSERT_TRUE(codec.validate(g, kMq135GasKey));
  TEST_ASSERT_FALSE(codec.validate(g, ProfileTarget::Mq135));
  TEST_ASSERT_TRUE(manager.stage_import(kMq135GasKey, g));
  TEST_ASSERT_FALSE(manager.commit(kMq135GasKey));
  TEST_ASSERT_TRUE(manager.preview(kMq135GasKey));
  TEST_ASSERT_TRUE(manager.commit(kMq135GasKey));
  TEST_ASSERT_NOT_NULL(runtime.mq_adc[1]);
  TEST_ASSERT_TRUE(runtime.mq_adc[1]->valid_for_conversion());
  TEST_ASSERT_EQUAL_UINT32(3000, runtime.mq_adc[1]->reference_mv);
  Mq135GasProfile decoded{};
  TEST_ASSERT_TRUE(manager.export_active(kMq135GasKey, restored));
  TEST_ASSERT_TRUE(codec.decode(restored, decoded));
  TEST_ASSERT_TRUE(decoded.valid()); TEST_ASSERT_EQUAL_MEMORY(g.bytes.data(), restored.bytes.data(), g.size);
  TEST_ASSERT_TRUE(manager.export_active(ProfileTarget::Mq135, restored));
  TEST_ASSERT_EQUAL_MEMORY(a.bytes.data(), restored.bytes.data(), a.size);
  p.manifest.profile_version = 2;
  TEST_ASSERT_TRUE(codec.encode(p, restored));
  TEST_ASSERT_TRUE(manager.stage_import(kMq135GasKey, restored));
  TEST_ASSERT_TRUE(manager.preview(kMq135GasKey));
  store.fail_next(MemoryProfileStore::Fault::WriteActivation);
  TEST_ASSERT_FALSE(manager.commit(kMq135GasKey));
  ProfileManager reboot(persist);
  TEST_ASSERT_TRUE(reboot.load(kMq135GasKey));
  TEST_ASSERT_TRUE(reboot.export_active(kMq135GasKey, restored));
  TEST_ASSERT_EQUAL_MEMORY(g.bytes.data(), restored.bytes.data(), g.size);
  TEST_ASSERT_TRUE(codec.encode(p, restored));
  TEST_ASSERT_TRUE(reboot.stage_import(kMq135GasKey, restored));
  TEST_ASSERT_TRUE(reboot.preview(kMq135GasKey));
  TEST_ASSERT_TRUE(reboot.commit(kMq135GasKey));
  TEST_ASSERT_TRUE(reboot.rollback(kMq135GasKey));
  TEST_ASSERT_TRUE(reboot.export_active(kMq135GasKey, restored));
  TEST_ASSERT_EQUAL_MEMORY(g.bytes.data(), restored.bytes.data(), g.size);
  g.bytes[kProfileHeaderBytes + 2] ^= 1;
  TEST_ASSERT_FALSE(codec.validate(g, kMq135GasKey));
}
void test_gas_profile_rejects_invalid_baseline_and_ratio_never_falls_back_to_one() {
  auto p = gas_fixture(); ProfileCodec codec{}; EncodedProfile encoded{};
  p.preparation_confirmed = false; TEST_ASSERT_FALSE(p.valid());
  TEST_ASSERT_FALSE(codec.encode(p, encoded));
  p = gas_fixture(); p.reference.rejected = 1; TEST_ASSERT_FALSE(p.valid());
  p = gas_fixture(); p.adc_hash.fill(0); TEST_ASSERT_FALSE(p.valid());
  float ratio{};
  TEST_ASSERT_TRUE(mq135_ratio(100, 200, ratio)); TEST_ASSERT_FLOAT_WITHIN(.0001, .5, ratio);
  TEST_ASSERT_FALSE(mq135_ratio(100, 0, ratio)); TEST_ASSERT_EQUAL_FLOAT(0, ratio);
  TEST_ASSERT_FALSE(mq135_ratio(INFINITY, 200, ratio));
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_electrical_independent_reference_and_no_double_divider);
  RUN_TEST(test_invalid_inputs_never_create_values);
  RUN_TEST(test_reference_is_bounded_timestamped_median_not_arbitrary_r0);
  RUN_TEST(test_stale_outlier_short_duration_and_invalid_reference_fail_closed);
  RUN_TEST(test_gas_profile_roundtrip_atomic_commit_reboot_rollback_and_adc_isolation);
  RUN_TEST(test_gas_profile_rejects_invalid_baseline_and_ratio_never_falls_back_to_one);
  return UNITY_END();
}
