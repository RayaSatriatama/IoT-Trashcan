#pragma once
#include <cassert>
#include "models/model_lifecycle.h"

// Valid TEST_ONLY model values reused from the native model suite.
namespace debug_model_fixture {
using namespace sigap::models;
ModelId id(const char* text) {
  ModelId value{};
  assert(set_model_id(value, text));
  return value;
}

MembershipFunction trapezoid(const char* name, float a, float b, float c,
                             float d) {
  MembershipFunction value{};
  value.id = id(name);
  value.shape = MembershipShape::Trapezoid;
  value.points = {a, b, c, d};
  return value;
}

MembershipFunction triangle(const char* name, float a, float b, float c) {
  MembershipFunction value{};
  value.id = id(name);
  value.shape = MembershipShape::Triangle;
  value.points = {a, b, c, c};
  return value;
}

FisModelProfile test_fis(uint32_t version = 1U) {
  FisModelProfile profile{};
  profile.manifest.kind = sigap::acquisition::ProfileKind::Fis;
  profile.manifest.schema_version = kModelProfileSchemaVersion;
  profile.manifest.profile_version = version;
  profile.manifest.target_id = static_cast<uint32_t>(ModelTarget::FisOrganic);
  profile.manifest.validity = sigap::acquisition::ProfileValidity::TestOnly;
  profile.deployment = ModelDeployment::TestOnly;
  profile.input_count = 2U;

  for (std::size_t input_index = 0U; input_index < profile.input_count;
       ++input_index) {
    auto& input = profile.inputs[input_index];
    input.id = input_index == 0U ? ModelInput::FillPercent
                                 : ModelInput::WeightUtilizationPercent;
    input.unit = ModelUnit::Percent;
    input.domain_min = 0.0F;
    input.domain_max = 100.0F;
    input.set_count = 3U;
    input.sets[0] = trapezoid("low", 0.0F, 0.0F, 30.0F, 50.0F);
    input.sets[1] = triangle("middle", 30.0F, 50.0F, 70.0F);
    input.sets[2] = trapezoid("high", 50.0F, 70.0F, 100.0F, 100.0F);
  }

  profile.output.id = id("collection");
  profile.output.unit = ModelUnit::Percent;
  profile.output.domain_min = 0.0F;
  profile.output.domain_max = 100.0F;
  profile.output.set_count = 3U;
  profile.output.sets[0] = trapezoid("normal", 0.0F, 0.0F, 30.0F, 50.0F);
  profile.output.sets[1] = triangle("warning", 30.0F, 50.0F, 70.0F);
  profile.output.sets[2] = trapezoid("lift", 50.0F, 70.0F, 100.0F, 100.0F);

  const uint8_t consequents[9]{0U, 1U, 2U, 1U, 1U, 2U, 2U, 2U, 2U};
  profile.rule_count = 9U;
  for (uint8_t rule_index = 0U; rule_index < profile.rule_count; ++rule_index) {
    auto& rule = profile.rules[rule_index];
    rule.id = static_cast<uint16_t>(rule_index + 1U);
    rule.antecedent_count = 2U;
    rule.antecedent_set_indices[0] = static_cast<uint8_t>(rule_index / 3U);
    rule.antecedent_set_indices[1] = static_cast<uint8_t>(rule_index % 3U);
    rule.antecedent_operator = AntecedentOperator::AndMinimum;
    rule.consequent_set_index = consequents[rule_index];
  }

  profile.label_count = 3U;
  profile.labels[0] = FisLabel{id("Normal"), 0.0F};
  profile.labels[1] = FisLabel{id("Warning"), 40.0F};
  profile.labels[2] = FisLabel{id("Lift"), 70.0F};
  profile.clipping = ClippingOperator::Minimum;
  profile.aggregation = AggregationOperator::Maximum;
  profile.centroid_grid_points = 201U;
  profile.area_epsilon = 0.0001F;
  return profile;
}

SafetyModelProfile test_safety(uint32_t version = 1U) {
  SafetyModelProfile profile{};
  profile.manifest.kind = sigap::acquisition::ProfileKind::Safety;
  profile.manifest.schema_version = kSafetyProfileSchemaVersion;
  profile.manifest.profile_version = version;
  profile.manifest.target_id = kSafetyB3TargetId;
  profile.manifest.validity = sigap::acquisition::ProfileValidity::TestOnly;
  profile.deployment = ModelDeployment::TestOnly;
  profile.channel_count = 2U;
  profile.channels[0] = SafetyChannelRule{
      HazardSignal::TemperatureC, true, 80.0F, 70.0F};
  profile.channels[1] = SafetyChannelRule{
      HazardSignal::Mq7AdcMillivolts, true, 2500.0F, 2200.0F};
  profile.clear_consecutive_cycles = 3U;
  return profile;
}
}

namespace sigap::models {
// Host-only RAM fault injection; explicit instantiation leaves production class
// definitions unchanged. Invalid-present state is not reachable via validated load.
auto& model53_entries(ModelProfileLifecycle& lifecycle) noexcept;
template <auto Member> struct Model53FaultAccess {
  friend auto& model53_entries(ModelProfileLifecycle& lifecycle) noexcept {
    return lifecycle.*Member;
  }
};
template struct Model53FaultAccess<&ModelProfileLifecycle::entries_>;
}
