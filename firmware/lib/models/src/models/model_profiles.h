#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "acquisition/profiles.h"

namespace sigap::models {

inline constexpr uint32_t kModelProfileSchemaVersion{1U};
inline constexpr uint32_t kSafetyProfileSchemaVersion{kModelProfileSchemaVersion};
inline constexpr std::size_t kModelIdBytes{16U};
inline constexpr std::size_t kMaxFisInputs{4U};
inline constexpr std::size_t kMaxSetsPerInput{4U};
inline constexpr std::size_t kMaxFisRules{32U};
inline constexpr std::size_t kMaxFisLabels{4U};
inline constexpr std::size_t kMaxSafetyChannels{4U};
inline constexpr uint16_t kMaxCentroidGridPoints{257U};
inline constexpr uint16_t kMaxSafetyClearCycles{255U};

enum class ModelTarget : uint32_t {
  Unknown = 0U,
  FisOrganic = 101U,
  FisInorganic = 102U,
  FisB3 = 103U,
  SafetyB3 = 104U,
};

inline constexpr uint32_t kSafetyB3TargetId{
    static_cast<uint32_t>(ModelTarget::SafetyB3)};

using ModelId = std::array<char, kModelIdBytes>;

bool set_model_id(ModelId& destination, const char* text) noexcept;
bool model_id_valid(const ModelId& value) noexcept;
bool model_id_equal(const ModelId& left, const ModelId& right) noexcept;

enum class ModelDeployment : uint8_t {
  Unconfigured = 0U,
  TestOnly = 1U,
  Production = 2U,
};

enum class ModelInput : uint8_t {
  FillPercent = 1U,
  WeightUtilizationPercent = 2U,
  TemperatureC = 3U,
  Mq7AdcMillivolts = 4U,
  Mq8AdcMillivolts = 5U,
};

enum class ModelUnit : uint8_t {
  Percent = 1U,
  Celsius = 2U,
  Millivolts = 3U,
};

enum class MembershipShape : uint8_t {
  Triangle = 1U,
  Trapezoid = 2U,
};

enum class AntecedentOperator : uint8_t {
  AndMinimum = 1U,
  OrMaximum = 2U,
};

enum class ClippingOperator : uint8_t { Minimum = 1U };
enum class AggregationOperator : uint8_t { Maximum = 1U };

struct MembershipFunction final {
  ModelId id{};
  MembershipShape shape{MembershipShape::Triangle};
  std::array<float, 4> points{};
};

struct FisInputDefinition final {
  ModelInput id{ModelInput::FillPercent};
  ModelUnit unit{ModelUnit::Percent};
  float domain_min{0.0F};
  float domain_max{0.0F};
  uint8_t set_count{0U};
  std::array<MembershipFunction, kMaxSetsPerInput> sets{};
};

struct FisOutputDefinition final {
  ModelId id{};
  ModelUnit unit{ModelUnit::Percent};
  float domain_min{0.0F};
  float domain_max{0.0F};
  uint8_t set_count{0U};
  std::array<MembershipFunction, kMaxSetsPerInput> sets{};
};

struct FisRuleDefinition final {
  uint16_t id{0U};
  uint8_t antecedent_count{0U};
  std::array<uint8_t, kMaxFisInputs> antecedent_set_indices{};
  AntecedentOperator antecedent_operator{AntecedentOperator::AndMinimum};
  uint8_t consequent_set_index{0U};
};

struct FisLabel final {
  ModelId id{};
  float minimum_inclusive{0.0F};
};

struct FisModelProfile final {
  acquisition::ProfileManifest manifest{};
  ModelDeployment deployment{ModelDeployment::Unconfigured};
  uint8_t input_count{0U};
  std::array<FisInputDefinition, kMaxFisInputs> inputs{};
  FisOutputDefinition output{};
  uint8_t rule_count{0U};
  std::array<FisRuleDefinition, kMaxFisRules> rules{};
  uint8_t label_count{0U};
  std::array<FisLabel, kMaxFisLabels> labels{};
  ClippingOperator clipping{ClippingOperator::Minimum};
  AggregationOperator aggregation{AggregationOperator::Maximum};
  uint16_t centroid_grid_points{0U};
  float area_epsilon{0.0F};

  acquisition::ProfileRef ref() const noexcept {
    return acquisition::to_ref(manifest);
  }
};

enum class HazardSignal : uint8_t {
  TemperatureC = 1U,
  Mq7AdcMillivolts = 2U,
  Mq8AdcMillivolts = 3U,
};

struct SafetyChannelRule final {
  HazardSignal signal{HazardSignal::TemperatureC};
  bool required{false};
  float alert_at_or_above{0.0F};
  float clear_below{0.0F};
};

struct SafetyModelProfile final {
  acquisition::ProfileManifest manifest{};
  ModelDeployment deployment{ModelDeployment::Unconfigured};
  uint8_t channel_count{0U};
  std::array<SafetyChannelRule, kMaxSafetyChannels> channels{};
  uint16_t clear_consecutive_cycles{0U};

  acquisition::ProfileRef ref() const noexcept {
    return acquisition::to_ref(manifest);
  }
};

enum class ModelValidationError : uint8_t {
  None = 0U,
  InvalidManifest,
  InvalidTarget,
  ValidityMismatch,
  ResourceLimit,
  NonFinite,
  InvalidDomain,
  InvalidMembership,
  CoverageGap,
  DuplicateIdentity,
  InvalidRule,
  IncompleteRules,
  InvalidOperator,
  InvalidCentroid,
  InvalidHysteresis,
  InvalidClearPolicy,
};

struct ModelValidationResult final {
  ModelValidationError error{ModelValidationError::InvalidManifest};
  uint16_t index{0U};

  constexpr bool valid() const noexcept {
    return error == ModelValidationError::None;
  }
};

ModelValidationResult validate_fis_model(
    const FisModelProfile& profile) noexcept;
ModelValidationResult validate_safety_model(
    const SafetyModelProfile& profile) noexcept;

struct ModelProfileKey final {
  acquisition::ProfileKind kind{acquisition::ProfileKind::Fis};
  ModelTarget target{ModelTarget::Unknown};

  friend constexpr bool operator==(const ModelProfileKey& left,
                                   const ModelProfileKey& right) noexcept {
    return left.kind == right.kind && left.target == right.target;
  }
};

bool valid_model_key(ModelProfileKey key) noexcept;

}  // namespace sigap::models
