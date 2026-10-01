#include <array>
#include <cstdint>
#include <cstdio>

#include <unity.h>

#include "acquisition/profile_store.h"
#include "acquisition/profile_manager.h"

namespace {

using namespace sigap::acquisition;

template <typename ManifestOwner>
void manifest(ProfileManifest& value, ProfileTarget target, uint32_t version) {
  value = ProfileManifest{};
  value.schema_version = 1U;
  value.profile_version = version;
  value.target_id = static_cast<uint32_t>(target);
  value.validity = ProfileValidity::Valid;
}

Hx711Profile hx(uint32_t version, ProfileTarget target = ProfileTarget::Hx711Organic) {
  Hx711Profile profile{};
  manifest<int>(profile.manifest, target, version);
  profile.counts_per_kg = 500.0F;
  profile.tare_offset_counts = 1000;
  return profile;
}

UltrasonicProfile ultrasonic(uint32_t version) {
  UltrasonicProfile profile{};
  manifest<int>(profile.manifest, ProfileTarget::UltrasonicOrganic, version);
  profile.d_empty_mm = 100.0F;
  profile.d_full_mm = 20.0F;
  profile.echo_mm_per_us = 0.1F;
  profile.overshoot_tolerance_mm = 2.0F;
  profile.echo_timeout_us = 30000U;
  profile.inter_sensor_gap_us = 60000U;
  return profile;
}

void test_profile_codec_round_trips_all_profile_families() {
  ProfileCodec codec{};
  EncodedProfile encoded{};
  Hx711Profile source = hx(1U);
  TEST_ASSERT_TRUE(codec.encode(source, encoded));
  Hx711Profile restored{};
  TEST_ASSERT_TRUE(codec.decode(encoded, restored));
  TEST_ASSERT_EQUAL_UINT32(1U, restored.manifest.profile_version);
  TEST_ASSERT_EQUAL_INT64(1000, restored.tare_offset_counts);
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 500.0F, restored.counts_per_kg);

  UltrasonicProfile us = ultrasonic(2U);
  TEST_ASSERT_TRUE(codec.encode(us, encoded));
  UltrasonicProfile us_restored{};
  TEST_ASSERT_TRUE(codec.decode(encoded, us_restored));
  TEST_ASSERT_EQUAL_UINT32(2U, us_restored.manifest.profile_version);
  TEST_ASSERT_EQUAL_UINT32(30000U, us_restored.echo_timeout_us);

  MqAdcProfile mq{};
  manifest<int>(mq.manifest, ProfileTarget::Mq4, 3U);
  mq.reference_mv = 3300U;
  mq.full_scale_counts = 4095U;
  mq.divider_numerator = 2U;
  mq.divider_denominator = 1U;
  TEST_ASSERT_TRUE(codec.encode(mq, encoded));
  MqAdcProfile mq_restored{};
  TEST_ASSERT_TRUE(codec.decode(encoded, mq_restored));
  TEST_ASSERT_EQUAL_UINT32(4095U, mq_restored.full_scale_counts);

  Mq7HeaterProfile heater{};
  heater.present = true;
  heater.schema_version = 1U;
  heater.profile_version = 4U;
  heater.target_id = static_cast<uint32_t>(ProfileTarget::Mq7);
  heater.preheat_us = 10U;
  heater.high_clean_us = 20U;
  heater.low_detect_us = 30U;
  TEST_ASSERT_TRUE(codec.encode(heater, encoded));
  Mq7HeaterProfile heater_restored{};
  TEST_ASSERT_TRUE(codec.decode(encoded, heater_restored));
  TEST_ASSERT_EQUAL_UINT64(30U, heater_restored.low_detect_us);

  Ds18b20Profile ds{};
  manifest<int>(ds.manifest, ProfileTarget::Ds18b20B3, 5U);
  ds.has_rom = true;
  ds.rom[0] = 0x28U;
  ds.resolution_bits = 10U;
  ds.offset_centi_c = -12;
  TEST_ASSERT_TRUE(codec.encode(ds, encoded));
  Ds18b20Profile ds_restored{};
  TEST_ASSERT_TRUE(codec.decode(encoded, ds_restored));
  TEST_ASSERT_TRUE(ds_restored.has_rom);
  TEST_ASSERT_EQUAL_UINT8(10U, ds_restored.resolution_bits);
  TEST_ASSERT_EQUAL_INT(-12, ds_restored.offset_centi_c);

  Sht31Profile sht{};
  manifest<int>(sht.manifest, ProfileTarget::Sht31_44, 6U);
  sht.address = 0x44U;
  sht.temperature_offset_centi_c = 5;
  sht.humidity_offset_centi_pct = -7;
  TEST_ASSERT_TRUE(codec.encode(sht, encoded));
  Sht31Profile sht_restored{};
  TEST_ASSERT_TRUE(codec.decode(encoded, sht_restored));
  TEST_ASSERT_EQUAL_UINT8(0x44U, sht_restored.address);
  TEST_ASSERT_EQUAL_INT(-7, sht_restored.humidity_offset_centi_pct);
}

void test_profile_commit_pointer_last_and_reload() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  EncodedProfile v1{};
  Hx711Profile first = hx(1U);
  TEST_ASSERT_TRUE(codec.encode(first, v1));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v1)));

  EncodedProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(persistence.load(ProfileTarget::Hx711Organic, loaded)));
  Hx711Profile loaded_v1{};
  TEST_ASSERT_TRUE(codec.decode(loaded, loaded_v1));
  TEST_ASSERT_EQUAL_UINT32(1U, loaded_v1.manifest.profile_version);

  EncodedProfile v2{};
  Hx711Profile second = hx(2U);
  second.counts_per_kg = 600.0F;
  TEST_ASSERT_TRUE(codec.encode(second, v2));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v2)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(persistence.rollback(ProfileTarget::Hx711Organic)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(persistence.load(ProfileTarget::Hx711Organic, loaded)));
  Hx711Profile restored{};
  TEST_ASSERT_TRUE(codec.decode(loaded, restored));
  TEST_ASSERT_EQUAL_UINT32(1U, restored.manifest.profile_version);
}

void test_failed_commit_keeps_previous_active() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  EncodedProfile v1{};
  TEST_ASSERT_TRUE(codec.encode(hx(1U), v1));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v1)));
  EncodedProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(hx(2U), v2));
  store.fail_next(MemoryProfileStore::Fault::WriteSlot);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::WriteFailed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v2)));
  EncodedProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(persistence.load(ProfileTarget::Hx711Organic, loaded)));
  Hx711Profile restored{};
  TEST_ASSERT_TRUE(codec.decode(loaded, restored));
  TEST_ASSERT_EQUAL_UINT32(1U, restored.manifest.profile_version);
}

void test_readback_and_pointer_faults_keep_previous_generation() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  EncodedProfile v1{};
  TEST_ASSERT_TRUE(codec.encode(hx(1U), v1));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v1)));

  EncodedProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(hx(2U), v2));
  store.fail_next(MemoryProfileStore::Fault::CorruptAfterWrite);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::IntegrityFailed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v2)));
  store.fail_next(MemoryProfileStore::Fault::WriteActivation);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::ActivationFailed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v2)));

  EncodedProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(persistence.load(ProfileTarget::Hx711Organic, loaded)));
  Hx711Profile restored{};
  TEST_ASSERT_TRUE(codec.decode(loaded, restored));
  TEST_ASSERT_EQUAL_UINT32(1U, restored.manifest.profile_version);
}

void test_target_mismatch_hash_schema_and_reset_are_rejected() {
  ProfileCodec codec{};
  EncodedProfile encoded{};
  Hx711Profile source = hx(1U, ProfileTarget::Hx711Organic);
  TEST_ASSERT_TRUE(codec.encode(source, encoded));
  TEST_ASSERT_FALSE(codec.validate(encoded, ProfileTarget::Hx711B3));
  encoded.bytes[encoded.size - 1U] ^= 0x01U;
  TEST_ASSERT_FALSE(codec.validate(encoded, ProfileTarget::Hx711Organic));

  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  TEST_ASSERT_TRUE(persistence.reset(ProfileTarget::Hx711Organic));
  EncodedProfile output{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound),
                        static_cast<int>(persistence.load(ProfileTarget::Hx711Organic, output)));
}

struct Audit final : IProfileAuditSink {
  uint32_t calls{0U};
  ProfileRef previous{};
  ProfileRef current{};
  void profile_activated(const ProfileRef& old_ref,
                         const ProfileRef& new_ref) noexcept override {
    ++calls;
    previous = old_ref;
    current = new_ref;
  }
};

void test_profile_manager_import_preview_commit_and_snapshot_identity() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  Audit audit{};
  ProfileManager manager(persistence, codec, &audit);
  TEST_ASSERT_TRUE(manager.load(ProfileTarget::Hx711Organic));
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(hx(1U), encoded));
  TEST_ASSERT_TRUE(manager.stage_import(ProfileTarget::Hx711Organic, encoded));
  TEST_ASSERT_TRUE(manager.validate(ProfileTarget::Hx711Organic));
  TEST_ASSERT_TRUE(manager.preview(ProfileTarget::Hx711Organic));
  TEST_ASSERT_TRUE(manager.commit(ProfileTarget::Hx711Organic));
  ActiveProfileSnapshot snapshot{};
  TEST_ASSERT_TRUE(manager.snapshot(ProfileTarget::Hx711Organic, snapshot));
  TEST_ASSERT_TRUE(snapshot.present);
  TEST_ASSERT_EQUAL_UINT32(1U, snapshot.ref.profile_version);
  TEST_ASSERT_EQUAL_UINT32(1U, audit.calls);
  EncodedProfile exported{};
  TEST_ASSERT_TRUE(manager.export_active(ProfileTarget::Hx711Organic, exported));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(encoded.hash().data(), exported.hash().data(),
                                encoded.hash().size());
}

void test_profile_memory_footprint_is_bounded() {
  std::printf("memory ProfileManifest=%zu Hx711Profile=%zu UltrasonicProfile=%zu "
              "Ds18b20Profile=%zu ProfileManager=%zu EncodedProfile=%zu\n",
              sizeof(ProfileManifest), sizeof(Hx711Profile),
              sizeof(UltrasonicProfile), sizeof(Ds18b20Profile),
              sizeof(ProfileManager), sizeof(EncodedProfile));
  TEST_ASSERT_TRUE(sizeof(EncodedProfile) <= 256U);
  TEST_ASSERT_TRUE(sizeof(ProfileManager) <= 12000U);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_profile_codec_round_trips_all_profile_families);
  RUN_TEST(test_profile_commit_pointer_last_and_reload);
  RUN_TEST(test_failed_commit_keeps_previous_active);
  RUN_TEST(test_readback_and_pointer_faults_keep_previous_generation);
  RUN_TEST(test_target_mismatch_hash_schema_and_reset_are_rejected);
  RUN_TEST(test_profile_manager_import_preview_commit_and_snapshot_identity);
  RUN_TEST(test_profile_memory_footprint_is_bounded);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
