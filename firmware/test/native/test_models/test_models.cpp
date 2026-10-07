#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <unity.h>

#include "models/model_codec.h"
#include "models/model_lifecycle.h"
#include "models/model_profiles.h"

namespace {

using namespace sigap::models;

ModelId id(const char* text) {
  ModelId value{};
  TEST_ASSERT_TRUE(set_model_id(value, text));
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

void test_fis_profile_accepts_complete_bounded_test_only_fixture() {
  const auto result = validate_fis_model(test_fis());
  TEST_ASSERT_TRUE(result.valid());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelValidationError::None),
                        static_cast<int>(result.error));
}

void test_fis_profile_rejects_nonfinite_domain_and_membership() {
  auto profile = test_fis();
  profile.inputs[0].domain_max = NAN;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::NonFinite),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.inputs[0].sets[1].points[2] = INFINITY;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::NonFinite),
      static_cast<int>(validate_fis_model(profile).error));
}

void test_fis_profile_rejects_shoulder_gap_and_incomplete_rule_coverage() {
  auto profile = test_fis();
  profile.inputs[0].sets[0].points = {0.0F, 20.0F, 10.0F, 50.0F};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidMembership),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.inputs[0].sets[1].points = {55.0F, 60.0F, 70.0F, 70.0F};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::CoverageGap),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.rule_count = 8U;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::IncompleteRules),
      static_cast<int>(validate_fis_model(profile).error));
}

void test_fis_profile_rejects_invalid_operator_grid_and_deployment_identity() {
  auto profile = test_fis();
  profile.centroid_grid_points = 1U;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidCentroid),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.clipping = static_cast<ClippingOperator>(9U);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidOperator),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.deployment = ModelDeployment::Production;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::ValidityMismatch),
      static_cast<int>(validate_fis_model(profile).error));
}

void test_fis_profile_rejects_semantic_input_unit_mismatches() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelUnit::Percent),
                        static_cast<int>(model_unit_for_input(
                            ModelInput::FillPercent)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelUnit::Percent),
                        static_cast<int>(model_unit_for_input(
                            ModelInput::WeightUtilizationPercent)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelUnit::Celsius),
                        static_cast<int>(model_unit_for_input(
                            ModelInput::TemperatureC)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelUnit::Millivolts),
                        static_cast<int>(model_unit_for_input(
                            ModelInput::Mq7AdcMillivolts)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelUnit::Millivolts),
                        static_cast<int>(model_unit_for_input(
                            ModelInput::Mq8AdcMillivolts)));

  auto profile = test_fis();
  profile.inputs[0].unit = ModelUnit::Celsius;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidUnit),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.inputs[1].unit = ModelUnit::Millivolts;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidUnit),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.inputs[1].id = ModelInput::TemperatureC;
  profile.inputs[1].unit = ModelUnit::Percent;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidUnit),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.inputs[1].id = ModelInput::Mq7AdcMillivolts;
  profile.inputs[1].unit = ModelUnit::Celsius;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidUnit),
      static_cast<int>(validate_fis_model(profile).error));

  profile = test_fis();
  profile.inputs[1].id = ModelInput::Mq8AdcMillivolts;
  profile.inputs[1].unit = ModelUnit::Percent;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidUnit),
      static_cast<int>(validate_fis_model(profile).error));
}

void test_safety_profile_rejects_invalid_hysteresis_duplicate_and_unbounded_clear() {
  TEST_ASSERT_TRUE(validate_safety_model(test_safety()).valid());

  auto profile = test_safety();
  profile.channels[0].clear_below = profile.channels[0].alert_at_or_above;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidHysteresis),
      static_cast<int>(validate_safety_model(profile).error));

  profile = test_safety();
  profile.channels[1].signal = profile.channels[0].signal;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::DuplicateIdentity),
      static_cast<int>(validate_safety_model(profile).error));

  profile = test_safety();
  profile.clear_consecutive_cycles = 0U;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ModelValidationError::InvalidClearPolicy),
      static_cast<int>(validate_safety_model(profile).error));
}

void test_codec_is_deterministic_and_round_trips_fis_and_safety() {
  ModelProfileCodec codec{};
  EncodedModelProfile first{};
  EncodedModelProfile second{};
  const auto fis = test_fis(7U);
  TEST_ASSERT_TRUE(codec.encode(fis, first));
  TEST_ASSERT_TRUE(codec.encode(fis, second));
  TEST_ASSERT_EQUAL_size_t(first.size, second.size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.bytes.data(), second.bytes.data(), first.size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.hash().data(), second.hash().data(),
                                first.hash().size());

  FisModelProfile restored_fis{};
  TEST_ASSERT_TRUE(codec.decode(first, restored_fis));
  TEST_ASSERT_EQUAL_UINT32(7U, restored_fis.manifest.profile_version);
  TEST_ASSERT_EQUAL_UINT8(2U, restored_fis.input_count);
  TEST_ASSERT_EQUAL_UINT8(9U, restored_fis.rule_count);
  TEST_ASSERT_EQUAL_STRING("middle", restored_fis.inputs[0].sets[1].id.data());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.hash().data(),
                                restored_fis.manifest.hash.data(),
                                first.hash().size());

  EncodedModelProfile safety_encoded{};
  TEST_ASSERT_TRUE(codec.encode(test_safety(4U), safety_encoded));
  SafetyModelProfile restored_safety{};
  TEST_ASSERT_TRUE(codec.decode(safety_encoded, restored_safety));
  TEST_ASSERT_EQUAL_UINT32(4U, restored_safety.manifest.profile_version);
  TEST_ASSERT_EQUAL_UINT16(3U, restored_safety.clear_consecutive_cycles);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HazardSignal::Mq7AdcMillivolts),
                        static_cast<int>(restored_safety.channels[1].signal));
}

void test_codec_rejects_corrupt_truncated_wrong_key_and_invalid_semantics() {
  ModelProfileCodec codec{};
  EncodedModelProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(), encoded));
  const ModelProfileKey fis_key{
      sigap::acquisition::ProfileKind::Fis, ModelTarget::FisOrganic};
  const ModelProfileKey safety_key{
      sigap::acquisition::ProfileKind::Safety, ModelTarget::SafetyB3};
  TEST_ASSERT_TRUE(codec.validate(encoded, fis_key));
  TEST_ASSERT_FALSE(codec.validate(encoded, safety_key));

  auto corrupt = encoded;
  corrupt.bytes[corrupt.size - 1U] ^= 0x80U;
  TEST_ASSERT_FALSE(codec.validate(corrupt, fis_key));
  auto truncated = encoded;
  --truncated.size;
  TEST_ASSERT_FALSE(codec.validate(truncated, fis_key));

  auto invalid = test_safety();
  invalid.channels[0].alert_at_or_above = NAN;
  TEST_ASSERT_FALSE(codec.encode(invalid, encoded));

  auto invalid_fis_enum = test_fis();
  invalid_fis_enum.inputs[0].unit = static_cast<ModelUnit>(99U);
  TEST_ASSERT_FALSE(codec.encode(invalid_fis_enum, encoded));

  invalid = test_safety();
  invalid.channels[0].signal = static_cast<HazardSignal>(99U);
  TEST_ASSERT_FALSE(codec.encode(invalid, encoded));
}

void test_codec_hash_covers_profile_provenance() {
  ModelProfileCodec codec{};
  auto first = test_fis();
  first.manifest.provenance.present = true;
  first.manifest.provenance.monotonic_us = 100U;
  auto second = first;
  second.manifest.provenance.monotonic_us = 101U;
  EncodedModelProfile first_encoded{};
  EncodedModelProfile second_encoded{};
  TEST_ASSERT_TRUE(codec.encode(first, first_encoded));
  TEST_ASSERT_TRUE(codec.encode(second, second_encoded));
  TEST_ASSERT_NOT_EQUAL(0, std::memcmp(first_encoded.hash().data(),
                                       second_encoded.hash().data(),
                                       first_encoded.hash().size()));

  FisModelProfile restored{};
  TEST_ASSERT_TRUE(codec.decode(first_encoded, restored));
  TEST_ASSERT_TRUE(restored.manifest.provenance.present);
  TEST_ASSERT_EQUAL_UINT64(100U, restored.manifest.provenance.monotonic_us);
}

void test_lifecycle_requires_validate_preview_and_preserves_active_on_failure() {
  FaultInjectableModelPersistence persistence{};
  ModelProfileCodec codec{};
  ModelProfileLifecycle lifecycle(persistence, codec);
  const ModelProfileKey key{
      sigap::acquisition::ProfileKind::Fis, ModelTarget::FisOrganic};
  TEST_ASSERT_TRUE(lifecycle.load(key));

  EncodedModelProfile v1{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(1U), v1));
  TEST_ASSERT_TRUE(lifecycle.stage(key, v1));
  TEST_ASSERT_FALSE(lifecycle.commit(key));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));

  EncodedModelProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(2U), v2));
  TEST_ASSERT_TRUE(lifecycle.stage(key, v2));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  persistence.fail_next(ModelPersistenceFault::Activate);
  TEST_ASSERT_FALSE(lifecycle.commit(key));

  ModelProfileLifecycle rebooted(persistence, codec);
  TEST_ASSERT_TRUE(rebooted.load(key));
  ModelProfileSnapshot snapshot{};
  TEST_ASSERT_TRUE(rebooted.snapshot(key, snapshot));
  TEST_ASSERT_TRUE(snapshot.present);
  TEST_ASSERT_EQUAL_UINT32(1U, snapshot.ref.profile_version);
}

void test_lifecycle_snapshot_is_immutable_across_successful_activation() {
  FaultInjectableModelPersistence persistence{};
  ModelProfileCodec codec{};
  ModelProfileLifecycle lifecycle(persistence, codec);
  const ModelProfileKey key{
      sigap::acquisition::ProfileKind::Safety, ModelTarget::SafetyB3};
  TEST_ASSERT_TRUE(lifecycle.load(key));

  EncodedModelProfile v1{};
  EncodedModelProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(test_safety(1U), v1));
  TEST_ASSERT_TRUE(codec.encode(test_safety(2U), v2));
  TEST_ASSERT_TRUE(lifecycle.stage(key, v1));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));
  ModelProfileSnapshot cycle_a{};
  TEST_ASSERT_TRUE(lifecycle.snapshot(key, cycle_a));

  TEST_ASSERT_TRUE(lifecycle.stage(key, v2));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));
  ModelProfileSnapshot cycle_b{};
  TEST_ASSERT_TRUE(lifecycle.snapshot(key, cycle_b));

  TEST_ASSERT_EQUAL_UINT32(1U, cycle_a.ref.profile_version);
  TEST_ASSERT_EQUAL_UINT32(2U, cycle_b.ref.profile_version);
  TEST_ASSERT_NOT_EQUAL(0, std::memcmp(cycle_a.ref.hash.data(),
                                       cycle_b.ref.hash.data(),
                                       cycle_a.ref.hash.size()));
}

void test_lifecycle_requires_storage_load_before_staging() {
  FaultInjectableModelPersistence persistence{};
  ModelProfileCodec codec{};
  ModelProfileLifecycle lifecycle(persistence, codec);
  const ModelProfileKey key{
      sigap::acquisition::ProfileKind::Fis, ModelTarget::FisOrganic};
  EncodedModelProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(), encoded));
  TEST_ASSERT_FALSE(lifecycle.stage(key, encoded));
  TEST_ASSERT_TRUE(lifecycle.load(key));
  TEST_ASSERT_TRUE(lifecycle.stage(key, encoded));
}

void test_lifecycle_rejects_non_monotonic_version_and_recovers_previous() {
  FaultInjectableModelPersistence persistence{};
  ModelProfileCodec codec{};
  ModelProfileLifecycle lifecycle(persistence, codec);
  const ModelProfileKey key{
      sigap::acquisition::ProfileKind::Fis, ModelTarget::FisOrganic};
  TEST_ASSERT_TRUE(lifecycle.load(key));
  EncodedModelProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(2U), v2));
  TEST_ASSERT_TRUE(lifecycle.stage(key, v2));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));

  EncodedModelProfile v1{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(1U), v1));
  TEST_ASSERT_FALSE(lifecycle.stage(key, v1));

  EncodedModelProfile v3{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(3U), v3));
  TEST_ASSERT_TRUE(lifecycle.stage(key, v3));
  TEST_ASSERT_TRUE(lifecycle.validate(key));
  TEST_ASSERT_TRUE(lifecycle.preview(key));
  TEST_ASSERT_TRUE(lifecycle.commit(key));
  persistence.corrupt_active(key);

  ModelProfileLifecycle rebooted(persistence, codec);
  TEST_ASSERT_TRUE(rebooted.load(key));
  ModelProfileSnapshot snapshot{};
  TEST_ASSERT_TRUE(rebooted.snapshot(key, snapshot));
  TEST_ASSERT_EQUAL_UINT32(2U, snapshot.ref.profile_version);
}

void test_persistence_does_not_promote_staged_candidate_after_activation_failure() {
  ModelProfileCodec codec{};
  FaultInjectableModelPersistence persistence(codec);
  const ModelProfileKey key{
      sigap::acquisition::ProfileKind::Fis, ModelTarget::FisOrganic};

  EncodedModelProfile v1{};
  EncodedModelProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(1U), v1));
  TEST_ASSERT_TRUE(codec.encode(test_fis(2U), v2));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelCommitResult::Committed),
                        static_cast<int>(persistence.commit(key, v1)));

  persistence.fail_next(ModelPersistenceFault::Activate);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelCommitResult::ActivationFailed),
                        static_cast<int>(persistence.commit(key, v2)));
  persistence.corrupt_activation(key);

  EncodedModelProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelLoadStatus::Invalid),
                        static_cast<int>(persistence.load(key, loaded)));
}

void test_persistence_recovers_only_committed_previous_generation() {
  ModelProfileCodec codec{};
  FaultInjectableModelPersistence persistence(codec);
  const ModelProfileKey key{
      sigap::acquisition::ProfileKind::Fis, ModelTarget::FisOrganic};
  EncodedModelProfile v1{};
  EncodedModelProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(1U), v1));
  TEST_ASSERT_TRUE(codec.encode(test_fis(2U), v2));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelCommitResult::Committed),
                        static_cast<int>(persistence.commit(key, v1)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelCommitResult::Committed),
                        static_cast<int>(persistence.commit(key, v2)));

  persistence.corrupt_active(key);
  EncodedModelProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelLoadStatus::Found),
                        static_cast<int>(persistence.load(key, loaded)));
  ModelProfileIdentity identity{};
  TEST_ASSERT_TRUE(codec.identity(loaded, identity));
  TEST_ASSERT_EQUAL_UINT32(1U, identity.profile_version);
}

void test_persistence_keeps_model_kind_and_target_keys_independent() {
  ModelProfileCodec codec{};
  FaultInjectableModelPersistence persistence(codec);
  const ModelProfileKey fis_key{
      sigap::acquisition::ProfileKind::Fis, ModelTarget::FisOrganic};
  const ModelProfileKey safety_key{
      sigap::acquisition::ProfileKind::Safety, ModelTarget::SafetyB3};
  EncodedModelProfile fis{};
  EncodedModelProfile safety{};
  TEST_ASSERT_TRUE(codec.encode(test_fis(1U), fis));
  TEST_ASSERT_TRUE(codec.encode(test_safety(1U), safety));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelCommitResult::Committed),
                        static_cast<int>(persistence.commit(fis_key, fis)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelCommitResult::Committed),
                        static_cast<int>(persistence.commit(safety_key, safety)));

  persistence.corrupt_active(fis_key);
  EncodedModelProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ModelLoadStatus::Found),
                        static_cast<int>(persistence.load(safety_key, loaded)));
  ModelProfileIdentity identity{};
  TEST_ASSERT_TRUE(codec.identity(loaded, identity));
  TEST_ASSERT_TRUE(identity.key == safety_key);
}

void test_model_profile_memory_is_bounded() {
  std::printf("memory FisModelProfile=%zu SafetyModelProfile=%zu "
              "EncodedModelProfile=%zu ModelProfileLifecycle=%zu\n",
              sizeof(FisModelProfile), sizeof(SafetyModelProfile),
              sizeof(EncodedModelProfile), sizeof(ModelProfileLifecycle));
  TEST_ASSERT_TRUE(sizeof(FisModelProfile) <= 2048U);
  TEST_ASSERT_TRUE(sizeof(SafetyModelProfile) <= 512U);
  TEST_ASSERT_TRUE(sizeof(EncodedModelProfile) <= 4096U);
  TEST_ASSERT_TRUE(sizeof(ModelProfileLifecycle) <= 20000U);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_fis_profile_accepts_complete_bounded_test_only_fixture);
  RUN_TEST(test_fis_profile_rejects_nonfinite_domain_and_membership);
  RUN_TEST(test_fis_profile_rejects_shoulder_gap_and_incomplete_rule_coverage);
  RUN_TEST(test_fis_profile_rejects_invalid_operator_grid_and_deployment_identity);
  RUN_TEST(test_fis_profile_rejects_semantic_input_unit_mismatches);
  RUN_TEST(test_safety_profile_rejects_invalid_hysteresis_duplicate_and_unbounded_clear);
  RUN_TEST(test_codec_is_deterministic_and_round_trips_fis_and_safety);
  RUN_TEST(test_codec_rejects_corrupt_truncated_wrong_key_and_invalid_semantics);
  RUN_TEST(test_codec_hash_covers_profile_provenance);
  RUN_TEST(test_lifecycle_requires_validate_preview_and_preserves_active_on_failure);
  RUN_TEST(test_lifecycle_snapshot_is_immutable_across_successful_activation);
  RUN_TEST(test_lifecycle_requires_storage_load_before_staging);
  RUN_TEST(test_lifecycle_rejects_non_monotonic_version_and_recovers_previous);
  RUN_TEST(test_persistence_does_not_promote_staged_candidate_after_activation_failure);
  RUN_TEST(test_persistence_recovers_only_committed_previous_generation);
  RUN_TEST(test_persistence_keeps_model_kind_and_target_keys_independent);
  RUN_TEST(test_model_profile_memory_is_bounded);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
