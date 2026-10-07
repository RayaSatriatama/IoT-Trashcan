#include <array>
#include <cstdint>
#include <type_traits>

#include <unity.h>

#include "processing/contracts.h"

namespace {

using namespace sigap::acquisition;
using namespace sigap::processing;

ConfigRef config_ref(uint64_t version, uint8_t hash_seed) {
  ConfigRef ref{};
  ref.schema_version = 1U;
  ref.config_version = version;
  ref.present = true;
  for (std::size_t index = 0U; index < ref.hash.size(); ++index) {
    ref.hash[index] = static_cast<uint8_t>(hash_seed + index);
  }
  return ref;
}

ProfileRef profile_ref(ProfileKind kind,
                       uint32_t target,
                       uint32_t version,
                       uint8_t hash_seed) {
  ProfileRef ref{};
  ref.kind = kind;
  ref.schema_version = 1U;
  ref.profile_version = version;
  ref.target_id = target;
  ref.present = true;
  for (std::size_t index = 0U; index < ref.hash.size(); ++index) {
    ref.hash[index] = static_cast<uint8_t>(hash_seed + index);
  }
  return ref;
}

SourceSampleId source_id(uint64_t sequence, uint8_t marker) {
  SourceSampleId id{};
  id.device_id.present = true;
  id.device_id.bytes[0] = marker;
  id.boot_id.present = true;
  id.boot_id.bytes[15] = static_cast<uint8_t>(marker + 1U);
  id.sequence = sequence;
  return id;
}

void test_source_ids_reject_overflow_without_truncation() {
  SourceSampleIds<2U> ids{};

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(ids.try_add(source_id(10U, 0x10U))));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(ids.try_add(source_id(11U, 0x20U))));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Full),
      static_cast<int>(ids.try_add(source_id(12U, 0x30U))));

  TEST_ASSERT_EQUAL_UINT32(2U, ids.size());
  TEST_ASSERT_TRUE(ids.overflowed());
  TEST_ASSERT_NOT_NULL(ids.at(0U));
  TEST_ASSERT_NOT_NULL(ids.at(1U));
  TEST_ASSERT_EQUAL_UINT64(10U, ids.at(0U)->sequence);
  TEST_ASSERT_EQUAL_UINT64(11U, ids.at(1U)->sequence);
  TEST_ASSERT_NULL(ids.at(2U));
}

void test_source_ids_reject_invalid_and_duplicate_identity() {
  SourceSampleIds<2U> ids{};
  SourceSampleId invalid{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Invalid),
      static_cast<int>(ids.try_add(invalid)));

  const auto first = source_id(UINT64_MAX, 0x42U);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(ids.try_add(first)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Duplicate),
      static_cast<int>(ids.try_add(first)));
  TEST_ASSERT_EQUAL_UINT32(1U, ids.size());
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ids.at(0U)->sequence);
}

void test_cycle_snapshot_copies_config_profile_and_source_identity() {
  auto active_config = config_ref(7U, 0x10U);
  ProfileRefSet profiles{};
  auto calibration =
      profile_ref(ProfileKind::Calibration, 1U, 4U, 0x30U);
  auto fis = profile_ref(ProfileKind::Fis, 0U, 2U, 0x50U);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(profiles.try_add(calibration)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(profiles.try_add(fis)));

  SourceSampleIds<3U> sources{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(sources.try_add(source_id(101U, 0x61U))));

  CycleSnapshot<3U> snapshot{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(SnapshotStatus::Captured),
      static_cast<int>(CycleSnapshot<3U>::capture(
          9000U, active_config, profiles, sources, snapshot)));

  active_config.config_version = 8U;
  active_config.hash[0] = 0xffU;
  calibration.profile_version = 5U;
  calibration.hash[0] = 0xeeU;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(sources.try_add(source_id(102U, 0x62U))));

  TEST_ASSERT_EQUAL_UINT64(9000U, snapshot.started_monotonic_us());
  TEST_ASSERT_EQUAL_UINT64(7U, snapshot.config_ref().config_version);
  TEST_ASSERT_EQUAL_HEX8(0x10U, snapshot.config_ref().hash[0]);
  TEST_ASSERT_EQUAL_UINT32(2U, snapshot.profile_refs().size());
  TEST_ASSERT_EQUAL_UINT32(4U,
                           snapshot.profile_refs().at(0U)->profile_version);
  TEST_ASSERT_EQUAL_HEX8(0x30U, snapshot.profile_refs().at(0U)->hash[0]);
  TEST_ASSERT_EQUAL_UINT32(1U, snapshot.source_sample_ids().size());
  TEST_ASSERT_EQUAL_UINT64(
      101U, snapshot.source_sample_ids().at(0U)->sequence);
}

void test_cycle_snapshot_rejects_absent_config_empty_or_overflowed_sources() {
  ProfileRefSet profiles{};
  SourceSampleIds<1U> empty_sources{};
  CycleSnapshot<1U> snapshot{};

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(SnapshotStatus::InvalidConfig),
      static_cast<int>(CycleSnapshot<1U>::capture(
          1U, ConfigRef{}, profiles, empty_sources, snapshot)));

  const auto config = config_ref(1U, 0x11U);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(SnapshotStatus::NoSources),
      static_cast<int>(CycleSnapshot<1U>::capture(
          1U, config, profiles, empty_sources, snapshot)));

  SourceSampleIds<1U> overflowed{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(overflowed.try_add(source_id(1U, 0x01U))));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Full),
      static_cast<int>(overflowed.try_add(source_id(2U, 0x02U))));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(SnapshotStatus::SourceOverflow),
      static_cast<int>(CycleSnapshot<1U>::capture(
          1U, config, profiles, overflowed, snapshot)));
}

void test_profile_refs_preserve_distinct_kind_target_version_and_hash() {
  ProfileRefSet profiles{};
  const auto fis = profile_ref(ProfileKind::Fis, 0U, 9U, 0x70U);
  const auto safety = profile_ref(ProfileKind::Safety, 3U, 11U, 0x90U);

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(profiles.try_add(fis)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Added),
      static_cast<int>(profiles.try_add(safety)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(CollectionStatus::Duplicate),
      static_cast<int>(profiles.try_add(fis)));

  TEST_ASSERT_EQUAL_UINT32(2U, profiles.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileKind::Fis),
                        static_cast<int>(profiles.at(0U)->kind));
  TEST_ASSERT_EQUAL_UINT32(9U, profiles.at(0U)->profile_version);
  TEST_ASSERT_EQUAL_HEX8(0x70U, profiles.at(0U)->hash[0]);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileKind::Safety),
                        static_cast<int>(profiles.at(1U)->kind));
  TEST_ASSERT_EQUAL_UINT32(3U, profiles.at(1U)->target_id);
  TEST_ASSERT_EQUAL_UINT32(11U, profiles.at(1U)->profile_version);
  TEST_ASSERT_EQUAL_HEX8(0x90U, profiles.at(1U)->hash[0]);
}

void test_quality_reason_contract_distinguishes_operational_readiness() {
  const InputDisposition ready{InputQuality::Ready, ProcessingReason::Ready};
  const InputDisposition stale{InputQuality::Stale,
                               ProcessingReason::InputStale};
  const InputDisposition calibrating{
      InputQuality::Calibrating, ProcessingReason::InputCalibrating};
  const InputDisposition contradictory{
      InputQuality::Ready, ProcessingReason::InputMissing};

  TEST_ASSERT_TRUE(ready.valid());
  TEST_ASSERT_TRUE(ready.operational());
  TEST_ASSERT_TRUE(stale.valid());
  TEST_ASSERT_FALSE(stale.operational());
  TEST_ASSERT_TRUE(calibrating.valid());
  TEST_ASSERT_FALSE(calibrating.operational());
  TEST_ASSERT_FALSE(contradictory.valid());
}

struct DummyFuzzyResult final {
  uint32_t score_milli{0U};
};

struct DummySafetyResult final {
  bool alert{false};
};

void test_fuzzy_and_safety_adapter_slots_are_distinct_bounded_types() {
  using FuzzySlot = FuzzyResultAdapterSlot<DummyFuzzyResult, 8U>;
  using SafetySlot = SafetyResultAdapterSlot<DummySafetyResult, 8U>;
  static_assert(!std::is_same<FuzzySlot, SafetySlot>::value,
                "FIS and Safety adapter slots must remain distinct");
  static_assert(sizeof(DummyFuzzyResult) <= FuzzySlot::capacity_bytes(),
                "FIS fixture must fit its bounded slot");
  static_assert(sizeof(DummySafetyResult) <= SafetySlot::capacity_bytes(),
                "Safety fixture must fit its bounded slot");

  FuzzySlot fuzzy{DummyFuzzyResult{32560U}};
  SafetySlot safety{DummySafetyResult{true}};
  TEST_ASSERT_EQUAL_UINT32(32560U, fuzzy.value().score_milli);
  TEST_ASSERT_TRUE(safety.value().alert);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_source_ids_reject_overflow_without_truncation);
  RUN_TEST(test_source_ids_reject_invalid_and_duplicate_identity);
  RUN_TEST(test_cycle_snapshot_copies_config_profile_and_source_identity);
  RUN_TEST(
      test_cycle_snapshot_rejects_absent_config_empty_or_overflowed_sources);
  RUN_TEST(test_profile_refs_preserve_distinct_kind_target_version_and_hash);
  RUN_TEST(test_quality_reason_contract_distinguishes_operational_readiness);
  RUN_TEST(test_fuzzy_and_safety_adapter_slots_are_distinct_bounded_types);
  return UNITY_END();
}
