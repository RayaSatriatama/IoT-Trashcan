#include "prototype_defaults.h"
#include "model_profiles.h"
#include "model_codec.h"

#include <cstdio>

namespace sigap::models {
namespace test {

bool validate_prototype_fis_builder() {
  FisModelProfile profile{};
  if (!build_prototype_organic_fis(profile)) {
    printf("[FAIL] build_prototype_organic_fis returned false\n");
    return false;
  }

  // Validate manifest
  if (profile.manifest.kind != acquisition::ProfileKind::Fis) {
    printf("[FAIL] manifest.kind != Fis\n");
    return false;
  }
  if (profile.manifest.target_id != static_cast<uint32_t>(ModelTarget::FisOrganic)) {
    printf("[FAIL] manifest.target_id != FisOrganic\n");
    return false;
  }
  if (profile.manifest.validity != acquisition::ProfileValidity::TestOnly) {
    printf("[FAIL] manifest.validity != TestOnly\n");
    return false;
  }
  if (profile.deployment != ModelDeployment::TestOnly) {
    printf("[FAIL] deployment != TestOnly\n");
    return false;
  }

  // Validate inputs
  if (profile.input_count != 4U) {
    printf("[FAIL] input_count != 4\n");
    return false;
  }
  if (profile.inputs[0].id != ModelInput::FillPercent) {
    printf("[FAIL] input[0] != FillPercent\n");
    return false;
  }
  if (profile.inputs[1].id != ModelInput::WeightUtilizationPercent) {
    printf("[FAIL] input[1] != WeightUtilizationPercent\n");
    return false;
  }
  if (profile.inputs[2].id != ModelInput::Ogi) {
    printf("[FAIL] input[2] != Ogi\n");
    return false;
  }
  if (profile.inputs[3].id != ModelInput::AccumulationHours) {
    printf("[FAIL] input[3] != AccumulationHours\n");
    return false;
  }

  // Validate input sets
  for (uint8_t i = 0; i < 4; ++i) {
    if (profile.inputs[i].set_count != 2U) {
      printf("[FAIL] input[%d] set_count != 2\n", i);
      return false;
    }
  }

  // Validate output
  if (profile.output.set_count != 3U) {
    printf("[FAIL] output set_count != 3\n");
    return false;
  }

  // Validate rules
  if (profile.rule_count != 16U) {
    printf("[FAIL] rule_count != 16\n");
    return false;
  }

  // Validate labels
  if (profile.label_count != 3U) {
    printf("[FAIL] label_count != 3\n");
    return false;
  }

  // Validate codec round-trip
  ModelProfileCodec codec{};
  EncodedModelProfile encoded{};
  if (!codec.encode(profile, encoded)) {
    printf("[FAIL] codec encode failed\n");
    return false;
  }

  FisModelProfile decoded{};
  if (!codec.decode(encoded, decoded)) {
    printf("[FAIL] codec decode failed\n");
    return false;
  }

  if (decoded.manifest.target_id != profile.manifest.target_id ||
      decoded.deployment != profile.deployment ||
      decoded.input_count != profile.input_count ||
      decoded.rule_count != profile.rule_count) {
    printf("[FAIL] codec round-trip mismatch\n");
    return false;
  }

  // Validate profile
  ModelValidationResult vr = validate_fis_model(profile);
  if (!vr.valid()) {
    printf("[FAIL] validate_fis_model error=%d index=%d\n",
           static_cast<int>(vr.error), vr.index);
    return false;
  }

  printf("[PASS] prototype FIS builder\n");
  return true;
}

bool validate_prototype_safety_builder() {
  SafetyModelProfile profile{};
  if (!build_prototype_organic_safety(profile)) {
    printf("[FAIL] build_prototype_organic_safety returned false\n");
    return false;
  }

  // Validate manifest
  if (profile.manifest.kind != acquisition::ProfileKind::Safety) {
    printf("[FAIL] manifest.kind != Safety\n");
    return false;
  }
  if (profile.manifest.target_id != static_cast<uint32_t>(ModelTarget::SafetyOrganic)) {
    printf("[FAIL] manifest.target_id != SafetyOrganic\n");
    return false;
  }
  if (profile.manifest.validity != acquisition::ProfileValidity::TestOnly) {
    printf("[FAIL] manifest.validity != TestOnly\n");
    return false;
  }
  if (profile.deployment != ModelDeployment::TestOnly) {
    printf("[FAIL] deployment != TestOnly\n");
    return false;
  }

  // Validate channel
  if (profile.channel_count != 1U) {
    printf("[FAIL] channel_count != 1\n");
    return false;
  }
  if (profile.channels[0].signal != HazardSignal::TemperatureC) {
    printf("[FAIL] channel[0] signal != TemperatureC\n");
    return false;
  }
  if (!profile.channels[0].required) {
    printf("[FAIL] channel[0] not required\n");
    return false;
  }
  if (profile.channels[0].alert_at_or_above != 60.0F) {
    printf("[FAIL] alert_at_or_above != 60\n");
    return false;
  }
  if (profile.channels[0].clear_below != 55.0F) {
    printf("[FAIL] clear_below != 55\n");
    return false;
  }

  // Validate codec round-trip
  ModelProfileCodec codec{};
  EncodedModelProfile encoded{};
  if (!codec.encode(profile, encoded)) {
    printf("[FAIL] codec encode failed\n");
    return false;
  }

  SafetyModelProfile decoded{};
  if (!codec.decode(encoded, decoded)) {
    printf("[FAIL] codec decode failed\n");
    return false;
  }

  if (decoded.manifest.target_id != profile.manifest.target_id ||
      decoded.deployment != profile.deployment ||
      decoded.channel_count != profile.channel_count) {
    printf("[FAIL] codec round-trip mismatch\n");
    return false;
  }

  // Validate profile
  ModelValidationResult vr = validate_safety_model(profile);
  if (!vr.valid()) {
    printf("[FAIL] validate_safety_model error=%d index=%d\n",
           static_cast<int>(vr.error), vr.index);
    return false;
  }

  printf("[PASS] prototype safety builder\n");
  return true;
}

bool validate_new_input_enums() {
  // ModelInput::Ogi = 6
  if (static_cast<uint8_t>(ModelInput::Ogi) != 6U) {
    printf("[FAIL] ModelInput::Ogi != 6\n");
    return false;
  }
  // ModelInput::AccumulationHours = 7
  if (static_cast<uint8_t>(ModelInput::AccumulationHours) != 7U) {
    printf("[FAIL] ModelInput::AccumulationHours != 7\n");
    return false;
  }
  // ModelUnit::Hours = 4
  if (static_cast<uint8_t>(ModelUnit::Hours) != 4U) {
    printf("[FAIL] ModelUnit::Hours != 4\n");
    return false;
  }
  // ModelTarget::SafetyOrganic = 105
  if (static_cast<uint32_t>(ModelTarget::SafetyOrganic) != 105U) {
    printf("[FAIL] ModelTarget::SafetyOrganic != 105\n");
    return false;
  }

  // model_unit_for_input
  if (model_unit_for_input(ModelInput::Ogi) != ModelUnit::Percent) {
    printf("[FAIL] model_unit_for_input(Ogi) != Percent\n");
    return false;
  }
  if (model_unit_for_input(ModelInput::AccumulationHours) != ModelUnit::Hours) {
    printf("[FAIL] model_unit_for_input(AccumulationHours) != Hours\n");
    return false;
  }

  printf("[PASS] new input enums\n");
  return true;
}

bool validate_valid_model_key() {
  // FIS organic should be valid
  if (!valid_model_key(ModelProfileKey{acquisition::ProfileKind::Fis, ModelTarget::FisOrganic})) {
    printf("[FAIL] FisOrganic key invalid\n");
    return false;
  }
  // Safety organic should be valid
  if (!valid_model_key(ModelProfileKey{acquisition::ProfileKind::Safety, ModelTarget::SafetyOrganic})) {
    printf("[FAIL] SafetyOrganic key invalid\n");
    return false;
  }
  // Safety B3 should still be valid
  if (!valid_model_key(ModelProfileKey{acquisition::ProfileKind::Safety, ModelTarget::SafetyB3})) {
    printf("[FAIL] SafetyB3 key invalid\n");
    return false;
  }

  printf("[PASS] valid_model_key\n");
  return true;
}

}  // namespace test
}  // namespace sigap::models

// Main test runner
int main() {
  bool all_pass = true;
  all_pass &= sigap::models::test::validate_prototype_fis_builder();
  all_pass &= sigap::models::test::validate_prototype_safety_builder();
  all_pass &= sigap::models::test::validate_new_input_enums();
  all_pass &= sigap::models::test::validate_valid_model_key();

  if (all_pass) {
    printf("\n=== ALL MODEL COMPONENT TESTS PASSED ===\n");
    return 0;
  } else {
    printf("\n=== SOME TESTS FAILED ===\n");
    return 1;
  }
}