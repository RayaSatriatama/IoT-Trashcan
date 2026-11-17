// S1 focused regression: RL-free normalized response and ratio helpers.
// All voltages here are explicitly supplied synthetic inputs, not measurements.
#include <cmath>
#include <limits>

#include <unity.h>

#include "acquisition/mq135_gas.h"

using namespace sigap::acquisition;

void setUp() {}
void tearDown() {}

// q = Vc / VAO - 1 and ratio = q / q0 must be computable with RL unknown.
void test_response_and_ratio_are_dimensionless_and_need_no_rl() {
  float q{0};
  TEST_ASSERT_TRUE(mq135_response(5000.0F, 2500.0F, q));
  TEST_ASSERT_FLOAT_WITHIN(1e-6F, 1.0F, q);

  float q0{0};
  TEST_ASSERT_TRUE(mq135_response(5000.0F, 2000.0F, q0));
  TEST_ASSERT_FLOAT_WITHIN(1e-6F, 1.5F, q0);

  float ratio{0};
  TEST_ASSERT_TRUE(mq135_ratio_normalized(q, q0, ratio));
  TEST_ASSERT_FLOAT_WITHIN(1e-6F, 2.0F / 3.0F, ratio);

  // Identical baseline and measurement conditions give ratio exactly 1.
  TEST_ASSERT_TRUE(mq135_ratio_normalized(q, q, ratio));
  TEST_ASSERT_EQUAL_FLOAT(1.0F, ratio);

  // Unit invariance: the helper is a ratio of voltages, so mV and V agree.
  float q_mv{0}, q_v{0};
  TEST_ASSERT_TRUE(mq135_response(5000.0F, 2500.0F, q_mv));
  TEST_ASSERT_TRUE(mq135_response(5.0F, 2.5F, q_v));
  TEST_ASSERT_FLOAT_WITHIN(1e-6F, q_mv, q_v);
}

// Zero, negative, non-finite and AO >= Vc must be unavailable, never a value.
void test_invalid_response_inputs_are_unavailable_not_zero_ppm() {
  float q{-1.0F};
  TEST_ASSERT_FALSE(mq135_response(5000.0F, 0.0F, q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);
  q = -1.0F;
  TEST_ASSERT_FALSE(mq135_response(5000.0F, -10.0F, q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);
  q = -1.0F;
  TEST_ASSERT_FALSE(mq135_response(0.0F, 2500.0F, q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);
  q = -1.0F;
  TEST_ASSERT_FALSE(mq135_response(-5000.0F, 2500.0F, q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);
  // AO equal to and above Vc has no physical resistance response.
  q = -1.0F;
  TEST_ASSERT_FALSE(mq135_response(5000.0F, 5000.0F, q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);
  q = -1.0F;
  TEST_ASSERT_FALSE(mq135_response(5000.0F, 5200.0F, q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);
  q = -1.0F;
  TEST_ASSERT_FALSE(mq135_response(std::numeric_limits<float>::quiet_NaN(), 2500.0F, q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);
  q = -1.0F;
  TEST_ASSERT_FALSE(mq135_response(5000.0F, std::numeric_limits<float>::infinity(), q));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, q);

  float ratio{-1.0F};
  TEST_ASSERT_FALSE(mq135_ratio_normalized(0.0F, 1.0F, ratio));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, ratio);
  ratio = -1.0F;
  TEST_ASSERT_FALSE(mq135_ratio_normalized(1.0F, 0.0F, ratio));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, ratio);
  ratio = -1.0F;
  TEST_ASSERT_FALSE(mq135_ratio_normalized(-1.0F, 1.0F, ratio));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, ratio);
  ratio = -1.0F;
  TEST_ASSERT_FALSE(mq135_ratio_normalized(1.0F, std::numeric_limits<float>::quiet_NaN(), ratio));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, ratio);
  ratio = -1.0F;
  TEST_ASSERT_FALSE(mq135_ratio_normalized(std::numeric_limits<float>::infinity(), 1.0F, ratio));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, ratio);
}

// Mandatory parent regression: RL/effective-load cancellation does NOT cancel a
// constant ADC voltage gain error. Fixture from PARENT_REVIEW_FLAGS.json.
void test_constant_adc_gain_error_is_not_cancelled_by_the_ratio() {
  constexpr float kGainError = 1.1F;
  float q0_true{0}, q_true{0}, ratio_true{0};
  TEST_ASSERT_TRUE(mq135_response(5000.0F, 2500.0F, q0_true));
  TEST_ASSERT_TRUE(mq135_response(5000.0F, 2000.0F, q_true));
  TEST_ASSERT_TRUE(mq135_ratio_normalized(q_true, q0_true, ratio_true));
  TEST_ASSERT_FLOAT_WITHIN(1e-6F, 1.5F, ratio_true);

  float q0_gain{0}, q_gain{0}, ratio_gain{0};
  TEST_ASSERT_TRUE(mq135_response(5000.0F, 2500.0F * kGainError, q0_gain));
  TEST_ASSERT_TRUE(mq135_response(5000.0F, 2000.0F * kGainError, q_gain));
  TEST_ASSERT_TRUE(mq135_ratio_normalized(q_gain, q0_gain, ratio_gain));

  TEST_ASSERT_FLOAT_WITHIN(1e-5F, 1.5555555555555554F, ratio_gain);
  TEST_ASSERT_GREATER_THAN_FLOAT(1.05F, ratio_gain);
  // The gain error survives the ratio: it must not be reported as 1.5.
  TEST_ASSERT_TRUE(std::fabs(ratio_gain - 1.5F) > 0.05F);
}

// The legacy absolute-resistance path keeps requiring a verified electrical model.
void test_absolute_ohms_still_require_verified_electrical() {
  MqAdcProfile adc{};
  adc.manifest.kind = ProfileKind::Adc;
  adc.manifest.target_id = 8;
  adc.manifest.profile_version = 1;
  adc.manifest.validity = ProfileValidity::TestOnly;
  adc.reference_mv = 3000;
  adc.full_scale_counts = 3000;
  adc.divider_numerator = 3;
  adc.divider_denominator = 5;

  Mq135Electrical electrical{};
  electrical.circuit_mv = 5000;
  electrical.module_load_ohm = 10000;
  electrical.divider_top_ohm = 10000;
  electrical.divider_bottom_ohm = 15000;
  electrical.topology = Mq135Topology::DirectLoaded;
  electrical.verified = false;
  const auto unverified = mq135_resistance(1500, adc, electrical);
  TEST_ASSERT_FALSE(unverified.ready());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135GasReason::ElectricalUnverified),
                        static_cast<int>(unverified.reason));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, unverified.rs_ohm);

  electrical.verified = true;
  const auto verified = mq135_resistance(1500, adc, electrical);
  TEST_ASSERT_TRUE(verified.ready());
  TEST_ASSERT_FLOAT_WITHIN(0.01F, 50000.0F / 7.0F, verified.rs_ohm);

  // Existing absolute ratio helper is preserved and never falls back to 1.
  float ratio{0};
  TEST_ASSERT_TRUE(mq135_ratio(100.0F, 200.0F, ratio));
  TEST_ASSERT_FLOAT_WITHIN(1e-4F, 0.5F, ratio);
  TEST_ASSERT_FALSE(mq135_ratio(100.0F, 0.0F, ratio));
  TEST_ASSERT_EQUAL_FLOAT(0.0F, ratio);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_response_and_ratio_are_dimensionless_and_need_no_rl);
  RUN_TEST(test_invalid_response_inputs_are_unavailable_not_zero_ppm);
  RUN_TEST(test_constant_adc_gain_error_is_not_cancelled_by_the_ratio);
  RUN_TEST(test_absolute_ohms_still_require_verified_electrical);
  return UNITY_END();
}
