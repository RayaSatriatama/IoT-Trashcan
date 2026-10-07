#include <cstddef>
#include <cstdint>

#include <unity.h>

#include "fuzzy/fis.h"

namespace {

using namespace sigap;

models::ModelId model_id(const char* value) {
  models::ModelId result{};
  TEST_ASSERT_TRUE(models::set_model_id(result, value));
  return result;
}

bool model_id_equals(const models::ModelId& id, const char* value) {
  models::ModelId expected{};
  return models::set_model_id(expected, value) &&
         models::model_id_equal(id, expected);
}

models::MembershipFunction triangle(const char* id,
                                    float left,
                                    float peak,
                                    float right) {
  models::MembershipFunction result{};
  result.id = model_id(id);
  result.shape = models::MembershipShape::Triangle;
  result.points = {left, peak, right, right};
  return result;
}

models::MembershipFunction trapezoid(const char* id,
                                     float left_foot,
                                     float left_shoulder,
                                     float right_shoulder,
                                     float right_foot) {
  models::MembershipFunction result{};
  result.id = model_id(id);
  result.shape = models::MembershipShape::Trapezoid;
  result.points = {left_foot, left_shoulder, right_shoulder, right_foot};
  return result;
}

template <typename Definition>
void define_three_sets(Definition& variable) {
  variable.domain_min = 0.0F;
  variable.domain_max = 100.0F;
  variable.set_count = 3U;
  variable.sets[0] = trapezoid("low", 0.0F, 0.0F, 30.0F, 50.0F);
  variable.sets[1] = triangle("medium", 30.0F, 50.0F, 70.0F);
  variable.sets[2] = trapezoid("high", 50.0F, 70.0F, 100.0F, 100.0F);
}

models::FisModelProfile bench_profile() {
  models::FisModelProfile model{};
  model.manifest.kind = acquisition::ProfileKind::Fis;
  model.manifest.schema_version = models::kModelProfileSchemaVersion;
  model.manifest.profile_version = 7U;
  model.manifest.target_id =
      static_cast<uint32_t>(models::ModelTarget::FisOrganic);
  model.manifest.validity = acquisition::ProfileValidity::TestOnly;
  for (std::size_t index = 0U; index < model.manifest.hash.size(); ++index) {
    model.manifest.hash[index] = static_cast<uint8_t>(0x20U + index);
  }
  model.deployment = models::ModelDeployment::TestOnly;
  model.input_count = 2U;
  model.inputs[0].id = models::ModelInput::FillPercent;
  model.inputs[0].unit = models::ModelUnit::Percent;
  define_three_sets(model.inputs[0]);
  model.inputs[1].id = models::ModelInput::WeightUtilizationPercent;
  model.inputs[1].unit = models::ModelUnit::Percent;
  define_three_sets(model.inputs[1]);

  model.output.id = model_id("collection");
  model.output.unit = models::ModelUnit::Percent;
  define_three_sets(model.output);

  model.rule_count = 9U;
  const uint8_t consequents[9]{0U, 1U, 2U, 1U, 1U,
                               2U, 2U, 2U, 2U};
  for (std::size_t index = 0U; index < model.rule_count; ++index) {
    auto& rule = model.rules[index];
    rule.id = static_cast<uint16_t>(index + 1U);
    rule.antecedent_count = 2U;
    rule.antecedent_set_indices[0] = static_cast<uint8_t>(index / 3U);
    rule.antecedent_set_indices[1] = static_cast<uint8_t>(index % 3U);
    rule.antecedent_operator = models::AntecedentOperator::AndMinimum;
    rule.consequent_set_index = consequents[index];
  }

  model.clipping = models::ClippingOperator::Minimum;
  model.aggregation = models::AggregationOperator::Maximum;
  model.centroid_grid_points = 257U;
  model.area_epsilon = 0.000001F;
  model.label_count = 3U;
  model.labels[0] = {model_id("Normal"), 0.0F};
  model.labels[1] = {model_id("Waspada"), 40.0F};
  model.labels[2] = {model_id("Angkat"), 70.0F};
  return model;
}

fuzzy::FisInputs ready_inputs(float fill, float weight) {
  fuzzy::FisInputs inputs{};
  inputs.count = 2U;
  inputs.values[0] = {fuzzy::FisInputState::Ready, fill};
  inputs.values[1] = {fuzzy::FisInputState::Ready, weight};
  return inputs;
}

const fuzzy::RuleTrace* find_rule(const fuzzy::FisResult& result,
                                  uint16_t id) {
  for (std::size_t index = 0U; index < result.trace.rule_count; ++index) {
    if (result.trace.rules[index].id == id) {
      return &result.trace.rules[index];
    }
  }
  return nullptr;
}

void test_membership_boundaries_support_triangles_and_shoulders() {
  const auto medium = triangle("medium", 30.0F, 50.0F, 70.0F);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.0F,
                           fuzzy::evaluate_membership(medium, 30.0F));
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.5F,
                           fuzzy::evaluate_membership(medium, 40.0F));
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 1.0F,
                           fuzzy::evaluate_membership(medium, 50.0F));
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.0F,
                           fuzzy::evaluate_membership(medium, 70.0F));

  const auto low = trapezoid("low", 0.0F, 0.0F, 30.0F, 50.0F);
  const auto high = trapezoid("high", 50.0F, 70.0F, 100.0F, 100.0F);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 1.0F,
                           fuzzy::evaluate_membership(low, 0.0F));
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.5F,
                           fuzzy::evaluate_membership(low, 40.0F));
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.5F,
                           fuzzy::evaluate_membership(high, 60.0F));
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 1.0F,
                           fuzzy::evaluate_membership(high, 100.0F));
}

void test_bench_40_40_matches_independent_centroid_and_rule_reference() {
  const auto model = bench_profile();
  TEST_ASSERT_TRUE(models::validate_fis_model(model).valid());
  const auto result =
      fuzzy::MamdaniEvaluator::evaluate(model, ready_inputs(40, 40));

  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisState::Ready),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::Ready),
                        static_cast<int>(result.reason));
  TEST_ASSERT_TRUE(result.score_present);
  TEST_ASSERT_FLOAT_WITHIN(0.08F, 1270.0F / 39.0F, result.score);
  TEST_ASSERT_TRUE(result.label_present);
  TEST_ASSERT_TRUE(model_id_equals(result.label, "Normal"));
  TEST_ASSERT_TRUE(result.trace_present);
  TEST_ASSERT_EQUAL_UINT32(6U, result.trace.membership_count);
  TEST_ASSERT_EQUAL_UINT32(4U, result.trace.rule_count);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.5F, find_rule(result, 1U)->strength);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.5F, find_rule(result, 2U)->strength);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.5F, find_rule(result, 4U)->strength);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.5F, find_rule(result, 5U)->strength);
  TEST_ASSERT_TRUE(result.trace.aggregate.present);
  TEST_ASSERT_TRUE(model_id_equals(result.trace.aggregate.output_id,
                                   "collection"));
  TEST_ASSERT_TRUE(result.trace.aggregate.centroid_present);
  TEST_ASSERT_FLOAT_WITHIN(0.08F, result.score,
                           result.trace.aggregate.centroid);
}

void test_explicit_or_rule_uses_maximum_antecedent_membership() {
  auto model = bench_profile();
  model.rules[0].antecedent_operator = models::AntecedentOperator::OrMaximum;
  TEST_ASSERT_TRUE(models::validate_fis_model(model).valid());
  const auto result =
      fuzzy::MamdaniEvaluator::evaluate(model, ready_inputs(10, 40));
  const auto* rule = find_rule(result, 1U);
  TEST_ASSERT_NOT_NULL(rule);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 1.0F, rule->strength);
}

void test_unconfigured_missing_stale_and_calibrating_are_not_ready() {
  const models::FisModelProfile absent{};
  auto result =
      fuzzy::MamdaniEvaluator::evaluate(absent, ready_inputs(40, 40));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisState::Unconfigured),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(fuzzy::FisReason::ModelNotConfigured),
      static_cast<int>(result.reason));
  TEST_ASSERT_FALSE(result.score_present);

  const auto model = bench_profile();
  auto inputs = ready_inputs(40, 40);
  inputs.values[0].state = fuzzy::FisInputState::Missing;
  result = fuzzy::MamdaniEvaluator::evaluate(model, inputs);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::InputMissing),
                        static_cast<int>(result.reason));
  TEST_ASSERT_FALSE(result.trace_present);

  inputs = ready_inputs(40, 40);
  inputs.values[0].state = fuzzy::FisInputState::Stale;
  result = fuzzy::MamdaniEvaluator::evaluate(model, inputs);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::InputStale),
                        static_cast<int>(result.reason));

  inputs = ready_inputs(40, 40);
  inputs.values[1].state = fuzzy::FisInputState::Calibrating;
  result = fuzzy::MamdaniEvaluator::evaluate(model, inputs);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(fuzzy::FisReason::InputCalibrating),
      static_cast<int>(result.reason));
}

void test_zero_area_is_unavailable_without_score_or_label() {
  auto model = bench_profile();
  model.area_epsilon = 99.0F;
  TEST_ASSERT_TRUE(models::validate_fis_model(model).valid());
  const auto result =
      fuzzy::MamdaniEvaluator::evaluate(model, ready_inputs(40, 40));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisState::Unavailable),
                        static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::ZeroArea),
                        static_cast<int>(result.reason));
  TEST_ASSERT_FALSE(result.score_present);
  TEST_ASSERT_FALSE(result.label_present);
  TEST_ASSERT_TRUE(result.trace_present);
  TEST_ASSERT_TRUE(result.trace.aggregate.present);
  TEST_ASSERT_FALSE(result.trace.aggregate.centroid_present);
}

void test_invalid_model_and_out_of_domain_input_are_unavailable() {
  auto model = bench_profile();
  model.inputs[0].sets[1].points = {30.0F, 20.0F, 70.0F, 70.0F};
  auto result =
      fuzzy::MamdaniEvaluator::evaluate(model, ready_inputs(40, 40));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::InvalidModel),
                        static_cast<int>(result.reason));
  TEST_ASSERT_FALSE(result.score_present);

  model = bench_profile();
  result = fuzzy::MamdaniEvaluator::evaluate(model, ready_inputs(101, 40));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(fuzzy::FisReason::InputOutOfDomain),
      static_cast<int>(result.reason));
}

void test_valid_model_exceeding_trace_rule_bound_is_rejected() {
  auto model = bench_profile();
  model.input_count = 3U;
  model.inputs[2] = model.inputs[0];
  model.inputs[2].id = models::ModelInput::TemperatureC;
  model.inputs[2].unit = models::ModelUnit::Celsius;
  model.rule_count = 27U;
  for (std::size_t index = 0U; index < model.rule_count; ++index) {
    auto& rule = model.rules[index];
    rule.id = static_cast<uint16_t>(index + 1U);
    rule.antecedent_count = 3U;
    rule.antecedent_set_indices[0] = static_cast<uint8_t>(index / 9U);
    rule.antecedent_set_indices[1] =
        static_cast<uint8_t>((index / 3U) % 3U);
    rule.antecedent_set_indices[2] = static_cast<uint8_t>(index % 3U);
    rule.antecedent_operator = models::AntecedentOperator::AndMinimum;
    rule.consequent_set_index = 0U;
  }
  TEST_ASSERT_TRUE(models::validate_fis_model(model).valid());
  fuzzy::FisInputs inputs{};
  inputs.count = 3U;
  for (std::size_t index = 0U; index < inputs.count; ++index) {
    inputs.values[index] = {fuzzy::FisInputState::Ready, 40.0F};
  }
  const auto result = fuzzy::MamdaniEvaluator::evaluate(model, inputs);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::TraceOverflow),
                        static_cast<int>(result.reason));
}

void test_result_copies_stable_model_identity() {
  auto model = bench_profile();
  const auto result =
      fuzzy::MamdaniEvaluator::evaluate(model, ready_inputs(40, 40));
  model.manifest.profile_version = 8U;
  model.manifest.hash[0] = 0xffU;

  TEST_ASSERT_EQUAL_UINT32(7U, result.trace.model_ref.version);
  TEST_ASSERT_EQUAL_HEX8(0x20U, result.trace.model_ref.hash[0]);
  TEST_ASSERT_TRUE(result.trace.model_ref.present);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(models::ModelDeployment::TestOnly),
                        static_cast<int>(result.model_state));
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_membership_boundaries_support_triangles_and_shoulders);
  RUN_TEST(test_bench_40_40_matches_independent_centroid_and_rule_reference);
  RUN_TEST(test_explicit_or_rule_uses_maximum_antecedent_membership);
  RUN_TEST(test_unconfigured_missing_stale_and_calibrating_are_not_ready);
  RUN_TEST(test_zero_area_is_unavailable_without_score_or_label);
  RUN_TEST(test_invalid_model_and_out_of_domain_input_are_unavailable);
  RUN_TEST(test_valid_model_exceeding_trace_rule_bound_is_rejected);
  RUN_TEST(test_result_copies_stable_model_identity);
  return UNITY_END();
}
