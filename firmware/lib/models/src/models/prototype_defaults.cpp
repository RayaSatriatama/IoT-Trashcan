#include "prototype_defaults.h"

#include <algorithm>
#include <cmath>

#include "model_profiles.h"

namespace sigap::models {

namespace {

// Helper to set a membership function with trapezoid shape
bool set_trap(MembershipFunction& out, const char* id, float p0, float p1, float p2, float p3,
              MembershipShape shape = MembershipShape::Trapezoid) noexcept {
    if (!set_model_id(out.id, id)) return false;
    out.shape = shape;
    out.points = {p0, p1, p2, p3};
    return true;
}

// Helper to set a membership function with triangle shape
bool set_tri(MembershipFunction& out, const char* id, float p0, float p1, float p2) noexcept {
    return set_trap(out, id, p0, p1, p2, p2, MembershipShape::Triangle);
}

// Fill and weight utilization sets: LOW trapezoid [0,0,30,70], HIGH trapezoid [30,70,100,100]
bool configure_percent_sets(FisInputDefinition& def, const char* name) noexcept {
    def.id = ModelInput::FillPercent; // will be overwritten by caller
    def.unit = ModelUnit::Percent;
    def.domain_min = 0.0F;
    def.domain_max = 100.0F;
    def.set_count = 2U;

    if (!set_trap(def.sets[0], "LOW", 0.0F, 0.0F, 30.0F, 70.0F)) return false;
    if (!set_trap(def.sets[1], "HIGH", 30.0F, 70.0F, 100.0F, 100.0F)) return false;
    return true;
}

// Accumulation hours sets: LOW [0,0,6,18], HIGH [6,18,24,24]
bool configure_hours_sets(FisInputDefinition& def) noexcept {
    def.id = ModelInput::AccumulationHours;
    def.unit = ModelUnit::Hours;
    def.domain_min = 0.0F;
    def.domain_max = 24.0F;
    def.set_count = 2U;

    if (!set_trap(def.sets[0], "LOW_H", 0.0F, 0.0F, 6.0F, 18.0F)) return false;
    if (!set_trap(def.sets[1], "HIGH_H", 6.0F, 18.0F, 24.0F, 24.0F)) return false;
    return true;
}

// OGI (organic gas index) sets: LOW [0,0,30,70], HIGH [30,70,100,100]
bool configure_ogi_sets(FisInputDefinition& def) noexcept {
    def.id = ModelInput::Ogi;
    def.unit = ModelUnit::Percent; // relative 0..100 index
    def.domain_min = 0.0F;
    def.domain_max = 100.0F;
    def.set_count = 2U;

    if (!set_trap(def.sets[0], "OGI_LOW", 0.0F, 0.0F, 30.0F, 70.0F)) return false;
    if (!set_trap(def.sets[1], "OGI_HIGH", 30.0F, 70.0F, 100.0F, 100.0F)) return false;
    return true;
}

// Output sets: LOW [0,0,20,45], MID triangle [20,50,80], HIGH [55,80,100,100]
bool configure_output_sets(FisOutputDefinition& out) noexcept {
    if (!set_model_id(out.id, "FIS_SCORE")) return false;
    out.unit = ModelUnit::Percent;
    out.domain_min = 0.0F;
    out.domain_max = 100.0F;
    out.set_count = 3U;

    if (!set_trap(out.sets[0], "LOW", 0.0F, 0.0F, 20.0F, 45.0F)) return false;
    if (!set_tri(out.sets[1], "MID", 20.0F, 50.0F, 80.0F)) return false;
    if (!set_trap(out.sets[2], "HIGH", 55.0F, 80.0F, 100.0F, 100.0F)) return false;
    return true;
}

// Build the 16-rule table (all binary combinations of 4 inputs with 2 sets each)
// Rule consequent: HIGH if FillHigh OR WeightHigh OR (OGIHigh AND HoursHigh)
//                 MID if OGIHigh OR HoursHigh
//                 LOW otherwise
bool configure_rules(FisModelProfile& profile) noexcept {
    // Input indices in profile.inputs: 0=Fill%, 1=WeightUtil%, 2=OGI%, 3=AccumHours
    // Set indices: 0=LOW, 1=HIGH
    constexpr uint8_t LOW = 0U;
    constexpr uint8_t HIGH = 1U;

    // All 16 combinations
    struct RuleSpec { uint8_t fill, weight, ogi, hours; uint8_t consequent; };
    RuleSpec specs[] = {
        {LOW, LOW, LOW, LOW, 0},  // LOW
        {LOW, LOW, LOW, HIGH, 1}, // MID (hoursHigh)
        {LOW, LOW, HIGH, LOW, 1}, // MID (ogiHigh)
        {LOW, LOW, HIGH, HIGH, 2},// HIGH (ogiHigh && hoursHigh)
        {LOW, HIGH, LOW, LOW, 2}, // HIGH (weightHigh)
        {LOW, HIGH, LOW, HIGH, 2},// HIGH
        {LOW, HIGH, HIGH, LOW, 2},// HIGH
        {LOW, HIGH, HIGH, HIGH, 2},// HIGH
        {HIGH, LOW, LOW, LOW, 2}, // HIGH (fillHigh)
        {HIGH, LOW, LOW, HIGH, 2},// HIGH
        {HIGH, LOW, HIGH, LOW, 2},// HIGH
        {HIGH, LOW, HIGH, HIGH, 2},// HIGH
        {HIGH, HIGH, LOW, LOW, 2},// HIGH
        {HIGH, HIGH, LOW, HIGH, 2},// HIGH
        {HIGH, HIGH, HIGH, LOW, 2},// HIGH
        {HIGH, HIGH, HIGH, HIGH, 2},// HIGH
    };

    profile.rule_count = 16U;
    for (std::size_t i = 0U; i < 16U; ++i) {
        auto& rule = profile.rules[i];
        rule.id = static_cast<uint16_t>(i + 1U);
        rule.antecedent_count = 4U;
        rule.antecedent_set_indices[0] = specs[i].fill;
        rule.antecedent_set_indices[1] = specs[i].weight;
        rule.antecedent_set_indices[2] = specs[i].ogi;
        rule.antecedent_set_indices[3] = specs[i].hours;
        rule.antecedent_operator = AntecedentOperator::AndMinimum;
        rule.consequent_set_index = specs[i].consequent; // 0=LOW, 1=MID, 2=HIGH
    }
    return true;
}

bool configure_labels(FisModelProfile& profile) noexcept {
    profile.label_count = 3U;
    if (!set_model_id(profile.labels[0].id, "LOW")) return false;
    profile.labels[0].minimum_inclusive = 0.0F;
    if (!set_model_id(profile.labels[1].id, "MID")) return false;
    profile.labels[1].minimum_inclusive = 20.0F;
    if (!set_model_id(profile.labels[2].id, "HIGH")) return false;
    profile.labels[2].minimum_inclusive = 55.0F;
    return true;
}

}  // namespace

bool build_prototype_organic_fis(FisModelProfile& output) noexcept {
    output = FisModelProfile{};

    // Manifest
    output.manifest.kind = acquisition::ProfileKind::Fis;
    output.manifest.schema_version = kModelProfileSchemaVersion;
    output.manifest.profile_version = 1U;
    output.manifest.target_id = static_cast<uint32_t>(ModelTarget::FisOrganic);
    output.manifest.validity = acquisition::ProfileValidity::TestOnly;
    output.deployment = ModelDeployment::TestOnly;

    // Input 0: Fill Percent
    if (!configure_percent_sets(output.inputs[0], "FILL")) return false;
    output.inputs[0].id = ModelInput::FillPercent;
    output.inputs[0].unit = ModelUnit::Percent;

    // Input 1: Weight Utilization Percent
    if (!configure_percent_sets(output.inputs[1], "WUTIL")) return false;
    output.inputs[1].id = ModelInput::WeightUtilizationPercent;
    output.inputs[1].unit = ModelUnit::Percent;

    // Input 2: OGI (Organic Gas Index) - relative 0..100
    if (!configure_ogi_sets(output.inputs[2])) return false;

    // Input 3: Accumulation Hours
    if (!configure_hours_sets(output.inputs[3])) return false;

    output.input_count = 4U;

    // Output
    if (!configure_output_sets(output.output)) return false;

    // Rules
    if (!configure_rules(output)) return false;

    // Labels
    if (!configure_labels(output)) return false;

    // Operators
    output.clipping = ClippingOperator::Minimum;
    output.aggregation = AggregationOperator::Maximum;
    output.centroid_grid_points = 101U;
    output.area_epsilon = 1e-4F;

    return true;
}

bool build_prototype_organic_safety(SafetyModelProfile& output) noexcept {
    output = SafetyModelProfile{};

    // Manifest
    output.manifest.kind = acquisition::ProfileKind::Safety;
    output.manifest.schema_version = kSafetyProfileSchemaVersion;
    output.manifest.profile_version = 1U;
    output.manifest.target_id = static_cast<uint32_t>(ModelTarget::SafetyOrganic);
    output.manifest.validity = acquisition::ProfileValidity::TestOnly;
    output.deployment = ModelDeployment::TestOnly;

    // One required temperature channel: alert >= 60C, clear < 55C, small consecutive clear
    output.channel_count = 1U;
    output.channels[0].signal = HazardSignal::TemperatureC;
    output.channels[0].required = true;
    output.channels[0].alert_at_or_above = 60.0F;
    output.channels[0].clear_below = 55.0F;
    output.clear_consecutive_cycles = 2U;

    return true;
}

}  // namespace sigap::models