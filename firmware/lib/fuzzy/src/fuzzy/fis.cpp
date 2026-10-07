#include "fuzzy/fis.h"

#include <algorithm>
#include <cmath>

namespace sigap::fuzzy {
namespace {

float rising(float value, float left, float right) noexcept {
  if (value <= left) return 0.0F;
  if (value >= right) return 1.0F;
  return (value - left) / (right - left);
}

float falling(float value, float left, float right) noexcept {
  if (value <= left) return 1.0F;
  if (value >= right) return 0.0F;
  return (right - value) / (right - left);
}

FisResult unavailable(const models::FisModelProfile& profile,
                      FisState state, FisReason reason) noexcept {
  FisResult result{};
  result.state = state;
  result.reason = reason;
  result.model_state = profile.deployment;
  return result;
}

FisReason input_reason(FisInputState state) noexcept {
  switch (state) {
    case FisInputState::Missing: return FisReason::InputMissing;
    case FisInputState::Stale: return FisReason::InputStale;
    case FisInputState::Calibrating: return FisReason::InputCalibrating;
    case FisInputState::Uncalibrated: return FisReason::InputUncalibrated;
    case FisInputState::Invalid: return FisReason::InputInvalid;
    case FisInputState::Ready: break;
  }
  return FisReason::InputInvalid;
}

}  // namespace

float evaluate_membership(const models::MembershipFunction& function,
                          float input) noexcept {
  if (!std::isfinite(input)) return 0.0F;
  const auto& p = function.points;
  if (function.shape == models::MembershipShape::Triangle) {
    if (input <= p[0] || input >= p[2]) return 0.0F;
    if (input == p[1]) return 1.0F;
    if (input < p[1]) return rising(input, p[0], p[1]);
    return falling(input, p[1], p[2]);
  }
  if (function.shape != models::MembershipShape::Trapezoid) return 0.0F;
  if (input < p[0] || input > p[3]) return 0.0F;
  if (p[0] == p[1] && input <= p[1]) return 1.0F;
  if (input < p[1]) return rising(input, p[0], p[1]);
  if (input <= p[2]) return 1.0F;
  if (p[2] == p[3] && input >= p[2]) return 1.0F;
  return falling(input, p[2], p[3]);
}

FisResult MamdaniEvaluator::evaluate(
    const models::FisModelProfile& profile, const FisInputs& inputs) noexcept {
  const bool unconfigured =
      profile.deployment == models::ModelDeployment::Unconfigured ||
      profile.manifest.profile_version == 0U;
  if (unconfigured) {
    return unavailable(profile, FisState::Unconfigured,
                       FisReason::ModelNotConfigured);
  }
  if (!models::validate_fis_model(profile).valid()) {
    return unavailable(profile, FisState::Unavailable, FisReason::InvalidModel);
  }
  if (inputs.count != profile.input_count ||
      inputs.count > inputs.values.size()) {
    return unavailable(profile, FisState::Unavailable, FisReason::InputInvalid);
  }
  for (std::size_t index = 0U; index < inputs.count; ++index) {
    if (inputs.values[index].state != FisInputState::Ready) {
      return unavailable(profile, FisState::Unavailable,
                         input_reason(inputs.values[index].state));
    }
    if (!std::isfinite(inputs.values[index].value) ||
        inputs.values[index].value < profile.inputs[index].domain_min ||
        inputs.values[index].value > profile.inputs[index].domain_max) {
      return unavailable(profile, FisState::Unavailable,
                         FisReason::InputOutOfDomain);
    }
  }
  std::size_t membership_capacity = 0U;
  for (std::size_t input_index = 0U; input_index < profile.input_count;
       ++input_index) {
    membership_capacity += profile.inputs[input_index].set_count;
  }
  if (profile.rule_count > kMaxTraceRules ||
      membership_capacity > kMaxTraceMemberships) {
    return unavailable(profile, FisState::Unavailable,
                       FisReason::TraceOverflow);
  }

  FisResult result{};
  result.state = FisState::Unavailable;
  result.reason = FisReason::ZeroArea;
  result.model_state = profile.deployment;
  result.trace_present = true;
  result.trace.model_ref.version = profile.manifest.profile_version;
  result.trace.model_ref.hash = profile.manifest.hash;
  result.trace.model_ref.present = true;
  result.trace.aggregate.present = true;
  result.trace.aggregate.output_id = profile.output.id;

  std::array<std::array<float, models::kMaxSetsPerInput>,
             models::kMaxFisInputs>
      memberships{};
  for (std::size_t input_index = 0U; input_index < profile.input_count;
       ++input_index) {
    const auto& definition = profile.inputs[input_index];
    for (std::size_t set_index = 0U; set_index < definition.set_count;
         ++set_index) {
      const float membership = evaluate_membership(
          definition.sets[set_index], inputs.values[input_index].value);
      memberships[input_index][set_index] = membership;
      auto& trace = result.trace.memberships[result.trace.membership_count++];
      trace.input = definition.id;
      trace.set_id = definition.sets[set_index].id;
      trace.value = membership;
    }
  }

  std::array<float, models::kMaxSetsPerInput> strengths{};
  for (std::size_t rule_index = 0U; rule_index < profile.rule_count;
       ++rule_index) {
    const auto& rule = profile.rules[rule_index];
    float strength = rule.antecedent_operator ==
                             models::AntecedentOperator::AndMinimum
                         ? 1.0F
                         : 0.0F;
    for (std::size_t input_index = 0U; input_index < rule.antecedent_count;
         ++input_index) {
      const float value = memberships[input_index]
          [rule.antecedent_set_indices[input_index]];
      strength = rule.antecedent_operator ==
                         models::AntecedentOperator::AndMinimum
                     ? std::min(strength, value)
                     : std::max(strength, value);
    }
    if (strength <= 0.0F) continue;
    if (result.trace.rule_count >= kMaxTraceRules) {
      result.reason = FisReason::TraceOverflow;
      result.trace_present = false;
      return result;
    }
    auto& trace = result.trace.rules[result.trace.rule_count++];
    trace.id = rule.id;
    trace.strength = strength;
    trace.output_id = profile.output.id;
    strengths[rule.consequent_set_index] =
        std::max(strengths[rule.consequent_set_index], strength);
  }

  const std::size_t grid = profile.centroid_grid_points;
  const float step = (profile.output.domain_max - profile.output.domain_min) /
                     static_cast<float>(grid - 1U);
  double area{0.0};
  double moment{0.0};
  float previous{0.0F};
  float previous_x{profile.output.domain_min};
  for (std::size_t point = 0U; point < grid; ++point) {
    const float x = profile.output.domain_min +
                    step * static_cast<float>(point);
    float aggregated{0.0F};
    for (std::size_t set_index = 0U; set_index < profile.output.set_count;
         ++set_index) {
      const float membership = evaluate_membership(
          profile.output.sets[set_index], x);
      aggregated = std::max(aggregated,
                            std::min(membership, strengths[set_index]));
    }
    if (point != 0U) {
      const double width = static_cast<double>(x - previous_x);
      area += width * (static_cast<double>(previous) + aggregated) * 0.5;
      moment += width * (static_cast<double>(previous_x) * previous +
                         static_cast<double>(x) * aggregated) * 0.5;
    }
    previous = aggregated;
    previous_x = x;
  }
  result.trace.aggregate.area = static_cast<float>(area);
  if (!std::isfinite(area) || area <= profile.area_epsilon) {
    return result;
  }
  const double centroid = moment / area;
  if (!std::isfinite(centroid)) return result;
  result.trace.aggregate.centroid_present = true;
  result.trace.aggregate.centroid = static_cast<float>(centroid);
  result.score_present = true;
  result.score = static_cast<float>(centroid);
  result.label_present = true;
  std::size_t selected = 0U;
  for (std::size_t index = 1U; index < profile.label_count; ++index) {
    if (centroid >= profile.labels[index].minimum_inclusive) selected = index;
  }
  result.label = profile.labels[selected].id;
  result.state = FisState::Ready;
  result.reason = FisReason::Ready;
  return result;
}

}  // namespace sigap::fuzzy
