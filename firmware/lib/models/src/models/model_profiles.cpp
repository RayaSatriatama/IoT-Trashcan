#include "models/model_profiles.h"

#include <algorithm>
#include <cmath>

namespace sigap::models {
namespace {

constexpr bool valid_deployment(ModelDeployment value) noexcept {
  return value == ModelDeployment::TestOnly ||
         value == ModelDeployment::Production;
}

bool manifest_valid(const acquisition::ProfileManifest& manifest,
                    acquisition::ProfileKind kind,
                    ModelTarget target,
                    ModelDeployment deployment) noexcept {
  if (manifest.kind != kind ||
      manifest.schema_version != kModelProfileSchemaVersion ||
      manifest.profile_version == 0U ||
      manifest.target_id != static_cast<uint32_t>(target) ||
      !valid_deployment(deployment)) {
    return false;
  }
  if (deployment == ModelDeployment::TestOnly) {
    return manifest.validity == acquisition::ProfileValidity::TestOnly;
  }
  return manifest.validity == acquisition::ProfileValidity::Valid;
}

bool finite(float value) noexcept { return std::isfinite(value); }

constexpr bool valid_input(ModelInput value) noexcept {
  switch (value) {
    case ModelInput::FillPercent:
    case ModelInput::WeightUtilizationPercent:
    case ModelInput::TemperatureC:
    case ModelInput::Mq7AdcMillivolts:
    case ModelInput::Mq8AdcMillivolts: return true;
  }
  return false;
}

constexpr bool valid_unit(ModelUnit value) noexcept {
  return value == ModelUnit::Percent || value == ModelUnit::Celsius ||
         value == ModelUnit::Millivolts;
}

constexpr bool valid_hazard(HazardSignal value) noexcept {
  return value == HazardSignal::TemperatureC ||
         value == HazardSignal::Mq7AdcMillivolts ||
         value == HazardSignal::Mq8AdcMillivolts;
}

ModelValidationResult result(ModelValidationError error,
                             std::size_t index = 0U) noexcept {
  return ModelValidationResult{error, static_cast<uint16_t>(index)};
}

ModelValidationResult validate_membership(const MembershipFunction& set,
                                          float domain_min,
                                          float domain_max) noexcept {
  if (!model_id_valid(set.id)) {
    return result(ModelValidationError::InvalidMembership);
  }
  for (const float point : set.points) {
    if (!finite(point)) return result(ModelValidationError::NonFinite);
    if (point < domain_min || point > domain_max) {
      return result(ModelValidationError::InvalidMembership);
    }
  }
  const auto& p = set.points;
  if (set.shape == MembershipShape::Triangle) {
    if (!(p[0] < p[1] && p[1] < p[2] && p[2] == p[3])) {
      return result(ModelValidationError::InvalidMembership);
    }
    return result(ModelValidationError::None);
  }
  if (set.shape != MembershipShape::Trapezoid ||
      !(p[0] <= p[1] && p[1] <= p[2] && p[2] <= p[3]) ||
      p[0] == p[3]) {
    return result(ModelValidationError::InvalidMembership);
  }
  return result(ModelValidationError::None);
}

template <typename Definition>
ModelValidationResult validate_definition_body(
    const Definition& definition) noexcept {
  if (!finite(definition.domain_min) || !finite(definition.domain_max)) {
    return result(ModelValidationError::NonFinite);
  }
  if (!(definition.domain_min < definition.domain_max)) {
    return result(ModelValidationError::InvalidDomain);
  }
  if (definition.set_count == 0U ||
      definition.set_count > definition.sets.size()) {
    return result(ModelValidationError::ResourceLimit);
  }
  for (std::size_t index = 0U; index < definition.set_count; ++index) {
    const auto membership = validate_membership(
        definition.sets[index], definition.domain_min, definition.domain_max);
    if (!membership.valid()) return result(membership.error, index);
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (model_id_equal(definition.sets[previous].id,
                         definition.sets[index].id)) {
        return result(ModelValidationError::DuplicateIdentity, index);
      }
    }
  }
  if (definition.sets[0].points[0] > definition.domain_min ||
      definition.sets[definition.set_count - 1U].points[3] <
          definition.domain_max) {
    return result(ModelValidationError::CoverageGap);
  }
  for (std::size_t index = 1U; index < definition.set_count; ++index) {
    if (definition.sets[index - 1U].points[3] <
        definition.sets[index].points[0]) {
      return result(ModelValidationError::CoverageGap, index);
    }
  }
  return result(ModelValidationError::None);
}

ModelValidationResult validate_definition(
    const FisInputDefinition& definition) noexcept {
  if (!valid_input(definition.id) || !valid_unit(definition.unit)) {
    return result(ModelValidationError::InvalidDomain);
  }
  return validate_definition_body(definition);
}

ModelValidationResult validate_definition(
    const FisOutputDefinition& definition) noexcept {
  if (!model_id_valid(definition.id)) {
    return result(ModelValidationError::InvalidDomain);
  }
  if (!valid_unit(definition.unit)) {
    return result(ModelValidationError::InvalidDomain);
  }
  return validate_definition_body(definition);
}

}  // namespace

bool set_model_id(ModelId& destination, const char* text) noexcept {
  destination.fill('\0');
  if (text == nullptr || text[0] == '\0') return false;
  std::size_t length{0U};
  while (text[length] != '\0') {
    if (length + 1U >= destination.size()) {
      destination.fill('\0');
      return false;
    }
    destination[length] = text[length];
    ++length;
  }
  return true;
}

bool model_id_valid(const ModelId& value) noexcept {
  if (value[0] == '\0') return false;
  return std::find(value.begin(), value.end(), '\0') != value.end();
}

bool model_id_equal(const ModelId& left, const ModelId& right) noexcept {
  return left == right;
}

bool valid_model_key(ModelProfileKey key) noexcept {
  if (key.kind == acquisition::ProfileKind::Fis) {
    return key.target == ModelTarget::FisOrganic ||
           key.target == ModelTarget::FisInorganic ||
           key.target == ModelTarget::FisB3;
  }
  return key.kind == acquisition::ProfileKind::Safety &&
         key.target == ModelTarget::SafetyB3;
}

ModelValidationResult validate_fis_model(
    const FisModelProfile& profile) noexcept {
  const auto target = static_cast<ModelTarget>(profile.manifest.target_id);
  if (!manifest_valid(profile.manifest, acquisition::ProfileKind::Fis,
                      target, profile.deployment)) {
    const bool identity_shape =
        profile.manifest.kind == acquisition::ProfileKind::Fis &&
        profile.manifest.schema_version == kModelProfileSchemaVersion &&
        profile.manifest.profile_version != 0U &&
        valid_model_key(ModelProfileKey{acquisition::ProfileKind::Fis, target});
    return result(identity_shape ? ModelValidationError::ValidityMismatch
                                 : ModelValidationError::InvalidManifest);
  }
  if (!valid_model_key(
          ModelProfileKey{acquisition::ProfileKind::Fis, target})) {
    return result(ModelValidationError::InvalidTarget);
  }
  if (profile.input_count == 0U ||
      profile.input_count > profile.inputs.size() ||
      profile.rule_count == 0U ||
      profile.rule_count > profile.rules.size() ||
      profile.label_count == 0U ||
      profile.label_count > profile.labels.size()) {
    return result(ModelValidationError::ResourceLimit);
  }
  for (std::size_t index = 0U; index < profile.input_count; ++index) {
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (profile.inputs[previous].id == profile.inputs[index].id) {
        return result(ModelValidationError::DuplicateIdentity, index);
      }
    }
    const auto input_validation =
        validate_definition(profile.inputs[index]);
    if (!input_validation.valid()) {
      return result(input_validation.error, index);
    }
  }
  const auto output_validation = validate_definition(profile.output);
  if (!output_validation.valid()) return output_validation;
  if (profile.clipping != ClippingOperator::Minimum ||
      profile.aggregation != AggregationOperator::Maximum) {
    return result(ModelValidationError::InvalidOperator);
  }
  if (profile.centroid_grid_points < 2U ||
      profile.centroid_grid_points > kMaxCentroidGridPoints ||
      !finite(profile.area_epsilon) || profile.area_epsilon <= 0.0F) {
    return result(ModelValidationError::InvalidCentroid);
  }

  std::size_t expected_rules{1U};
  for (std::size_t index = 0U; index < profile.input_count; ++index) {
    expected_rules *= profile.inputs[index].set_count;
  }
  if (expected_rules != profile.rule_count || expected_rules > kMaxFisRules) {
    return result(ModelValidationError::IncompleteRules);
  }
  std::array<bool, kMaxFisRules> combinations{};
  for (std::size_t rule_index = 0U; rule_index < profile.rule_count;
       ++rule_index) {
    const auto& rule = profile.rules[rule_index];
    if (rule.id == 0U || rule.antecedent_count != profile.input_count ||
        (rule.antecedent_operator != AntecedentOperator::AndMinimum &&
         rule.antecedent_operator != AntecedentOperator::OrMaximum) ||
        rule.consequent_set_index >= profile.output.set_count) {
      return result(ModelValidationError::InvalidRule, rule_index);
    }
    for (std::size_t previous = 0U; previous < rule_index; ++previous) {
      if (profile.rules[previous].id == rule.id) {
        return result(ModelValidationError::DuplicateIdentity, rule_index);
      }
    }
    std::size_t combination{0U};
    for (std::size_t input_index = 0U; input_index < profile.input_count;
         ++input_index) {
      if (rule.antecedent_set_indices[input_index] >=
          profile.inputs[input_index].set_count) {
        return result(ModelValidationError::InvalidRule, rule_index);
      }
      combination = combination * profile.inputs[input_index].set_count +
                    rule.antecedent_set_indices[input_index];
    }
    if (combination >= combinations.size() || combinations[combination]) {
      return result(ModelValidationError::IncompleteRules, rule_index);
    }
    combinations[combination] = true;
  }

  for (std::size_t index = 0U; index < profile.label_count; ++index) {
    const auto& label = profile.labels[index];
    if (!model_id_valid(label.id) || !finite(label.minimum_inclusive) ||
        label.minimum_inclusive < profile.output.domain_min ||
        label.minimum_inclusive > profile.output.domain_max ||
        (index > 0U &&
         !(profile.labels[index - 1U].minimum_inclusive <
           label.minimum_inclusive))) {
      return result(ModelValidationError::InvalidDomain, index);
    }
    if (index == 0U && label.minimum_inclusive != profile.output.domain_min) {
      return result(ModelValidationError::CoverageGap, index);
    }
  }
  return result(ModelValidationError::None);
}

ModelValidationResult validate_safety_model(
    const SafetyModelProfile& profile) noexcept {
  if (!manifest_valid(profile.manifest, acquisition::ProfileKind::Safety,
                      ModelTarget::SafetyB3, profile.deployment)) {
    const bool identity_shape =
        profile.manifest.kind == acquisition::ProfileKind::Safety &&
        profile.manifest.schema_version == kSafetyProfileSchemaVersion &&
        profile.manifest.profile_version != 0U &&
        profile.manifest.target_id == kSafetyB3TargetId;
    return result(identity_shape ? ModelValidationError::ValidityMismatch
                                 : ModelValidationError::InvalidManifest);
  }
  if (profile.channel_count == 0U ||
      profile.channel_count > profile.channels.size()) {
    return result(ModelValidationError::ResourceLimit);
  }
  if (profile.clear_consecutive_cycles == 0U ||
      profile.clear_consecutive_cycles > kMaxSafetyClearCycles) {
    return result(ModelValidationError::InvalidClearPolicy);
  }
  bool has_required{false};
  for (std::size_t index = 0U; index < profile.channel_count; ++index) {
    const auto& channel = profile.channels[index];
    if (!valid_hazard(channel.signal)) {
      return result(ModelValidationError::InvalidDomain, index);
    }
    if (!finite(channel.alert_at_or_above) || !finite(channel.clear_below)) {
      return result(ModelValidationError::NonFinite, index);
    }
    if (!(channel.clear_below < channel.alert_at_or_above)) {
      return result(ModelValidationError::InvalidHysteresis, index);
    }
    has_required = has_required || channel.required;
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (profile.channels[previous].signal == channel.signal) {
        return result(ModelValidationError::DuplicateIdentity, index);
      }
    }
  }
  return has_required ? result(ModelValidationError::None)
                      : result(ModelValidationError::InvalidClearPolicy);
}

}  // namespace sigap::models
