#include <cmath>
#include <cstdint>
#include <cstdio>

#include <unity.h>

#include "models/model_profiles.h"
#include "safety/safety.h"

namespace {

using namespace sigap::safety;
using namespace sigap::models;

SafetyModelProfile test_profile() {
  SafetyModelProfile profile{};
  profile.manifest.kind = sigap::acquisition::ProfileKind::Safety;
  profile.manifest.schema_version = kSafetyProfileSchemaVersion;
  profile.manifest.profile_version = 3U;
  profile.manifest.target_id = kSafetyB3TargetId;
  profile.manifest.validity = sigap::acquisition::ProfileValidity::TestOnly;
  profile.deployment = ModelDeployment::TestOnly;
  profile.clear_consecutive_cycles = 2U;
  profile.channel_count = 2U;
  profile.channels[0] = SafetyChannelRule{
      HazardSignal::TemperatureC, true, 60.0F, 50.0F};
  profile.channels[1] = SafetyChannelRule{
      HazardSignal::Mq7AdcMillivolts, true, 2200.0F, 1800.0F};
  return profile;
}

HazardInputFrame ready_inputs(float temperature_c, float mq7_mv) {
  HazardInputFrame frame{};
  frame.count = 2U;
  frame.values[0] = HazardValue{
      HazardSignal::TemperatureC, HazardInputState::Ready, temperature_c};
  frame.values[1] = HazardValue{
      HazardSignal::Mq7AdcMillivolts, HazardInputState::Ready, mq7_mv};
  return frame;
}

void test_unconfigured_and_invalid_input_never_become_clear() {
  SafetyEvaluator evaluator{};
  const auto inputs = ready_inputs(25.0F, 300.0F);

  auto result = evaluator.evaluate(nullptr, inputs);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Unconfigured),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyReason::SafetyNotConfigured),
                        static_cast<int>(result.reason));

  auto profile = test_profile();
  auto invalid = inputs;
  invalid.values[0].state = HazardInputState::Invalid;
  result = evaluator.evaluate(&profile, invalid);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Fault),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyReason::HazardInputInvalid),
                        static_cast<int>(result.reason));

  auto missing = inputs;
  missing.values[1].state = HazardInputState::Missing;
  result = evaluator.evaluate(&profile, missing);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Unavailable),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyReason::HazardInputMissing),
                        static_cast<int>(result.reason));
}

void test_alert_latches_until_validated_consecutive_clear_cycles() {
  auto profile = test_profile();
  SafetyEvaluator evaluator{};

  auto result = evaluator.evaluate(&profile, ready_inputs(65.0F, 300.0F));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Alert),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyReason::HazardThresholdExceeded),
                        static_cast<int>(result.reason));
  TEST_ASSERT_TRUE(evaluator.alert_latched());

  result = evaluator.evaluate(&profile, ready_inputs(55.0F, 300.0F));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Alert),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyReason::AlertLatched),
                        static_cast<int>(result.reason));

  result = evaluator.evaluate(&profile, ready_inputs(45.0F, 300.0F));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Alert),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_UINT8(1U, result.trace.clear_streak);

  result = evaluator.evaluate(&profile, ready_inputs(45.0F, 300.0F));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Clear),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(SafetyReason::AllValidatedChannelsClear),
      static_cast<int>(result.reason));
  TEST_ASSERT_FALSE(evaluator.alert_latched());
}

void test_invalid_input_during_latch_does_not_clear_alert() {
  auto profile = test_profile();
  SafetyEvaluator evaluator{};
  evaluator.evaluate(&profile, ready_inputs(65.0F, 300.0F));

  auto invalid = ready_inputs(45.0F, 300.0F);
  invalid.values[0].state = HazardInputState::Stale;
  const auto result = evaluator.evaluate(&profile, invalid);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Unavailable),
                        static_cast<int>(result.state));
  TEST_ASSERT_TRUE(evaluator.alert_latched());
  TEST_ASSERT_EQUAL_UINT8(0U, result.trace.clear_streak);

  // A collection/FIS score is intentionally absent from this API. Only a
  // validated hazard frame can advance the safety clear policy.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Alert),
                        static_cast<int>(
                            evaluator.evaluate(
                                &profile, ready_inputs(45.0F, 300.0F)).state));
}

void test_profile_validation_rejects_unsafe_or_ambiguous_models() {
  auto profile = test_profile();
  TEST_ASSERT_TRUE(validate_safety_model(profile).valid());

  profile.deployment = ModelDeployment::Unconfigured;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::ValidityMismatch),
      static_cast<int>(validate_safety_model(profile).error));
  profile = test_profile();
  profile.channels[0].clear_below = profile.channels[0].alert_at_or_above;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidHysteresis),
      static_cast<int>(validate_safety_model(profile).error));
  profile = test_profile();
  profile.channels[1].signal = profile.channels[0].signal;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::DuplicateIdentity),
      static_cast<int>(validate_safety_model(profile).error));
  profile = test_profile();
  profile.channels[0].alert_at_or_above = NAN;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelValidationError::NonFinite),
                        static_cast<int>(validate_safety_model(profile).error));
}

void test_trace_is_bounded_and_carries_model_provenance() {
  auto profile = test_profile();
  for (std::size_t i = 0U; i < profile.manifest.hash.size(); ++i) {
    profile.manifest.hash[i] = static_cast<uint8_t>(0x20U + i);
  }
  SafetyEvaluator evaluator{};
  const auto result = evaluator.evaluate(&profile, ready_inputs(25.0F, 300.0F));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Clear),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_UINT8(2U, result.trace.evidence_count);
  TEST_ASSERT_EQUAL_UINT32(3U, result.trace.model_ref.profile_version);
  TEST_ASSERT_EQUAL_HEX8(0x20U, result.trace.model_ref.hash[0]);
  TEST_ASSERT_LESS_OR_EQUAL_UINT8(kSafetyTraceChannels,
                                 result.trace.evidence_count);
}

void test_model_activation_never_clears_existing_alert() {
  auto v1 = test_profile();
  SafetyEvaluator evaluator{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(SafetyState::Alert),
      static_cast<int>(
          evaluator.evaluate(&v1, ready_inputs(65.0F, 300.0F)).state));

  auto v2 = test_profile();
  v2.manifest.profile_version = 4U;
  v2.manifest.hash[0] = 0x44U;
  const auto first_clear = evaluator.evaluate(&v2, ready_inputs(45.0F, 300.0F));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SafetyState::Alert),
                        static_cast<int>(first_clear.state));
  TEST_ASSERT_TRUE(evaluator.alert_latched());
  TEST_ASSERT_EQUAL_UINT8(1U, first_clear.trace.clear_streak);
}

void test_safety_runtime_state_is_bounded() {
  std::printf("memory SafetyEvaluator=%zu SafetyResult=%zu SafetyTrace=%zu\n",
              sizeof(SafetyEvaluator), sizeof(SafetyResult), sizeof(SafetyTrace));
  TEST_ASSERT_TRUE(sizeof(SafetyEvaluator) <= 64U);
  TEST_ASSERT_TRUE(sizeof(SafetyResult) <= 256U);
  TEST_ASSERT_TRUE(sizeof(SafetyTrace) <= 256U);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unconfigured_and_invalid_input_never_become_clear);
  RUN_TEST(test_alert_latches_until_validated_consecutive_clear_cycles);
  RUN_TEST(test_invalid_input_during_latch_does_not_clear_alert);
  RUN_TEST(test_profile_validation_rejects_unsafe_or_ambiguous_models);
  RUN_TEST(test_trace_is_bounded_and_carries_model_provenance);
  RUN_TEST(test_model_activation_never_clears_existing_alert);
  RUN_TEST(test_safety_runtime_state_is_bounded);
  return UNITY_END();
}
