#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <unity.h>

#include "acquisition/profile_store.h"
#include "acquisition/profile_manager.h"
#include "telemetry/sha256.h"

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

void test_default_template_store_is_absent_only_and_never_active_valid() {
  MemoryProfileStore store{};
  ProfileCodec codec{};
  const ProfileKey key{ProfileKind::Calibration, ProfileTarget::Hx711Organic};
  Hx711Profile unresolved{};
  manifest<int>(unresolved.manifest, key.target, 1U);
  unresolved.manifest.validity = ProfileValidity::Invalid;
  EncodedProfile template_bytes{};
  TEST_ASSERT_TRUE(codec.encode(unresolved, template_bytes));
  TEST_ASSERT_TRUE(codec.validate_template(template_bytes, key));
  TEST_ASSERT_FALSE(codec.validate(template_bytes, key));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound), static_cast<int>(store.read_default(key, template_bytes)));
  TEST_ASSERT_TRUE(codec.encode(unresolved, template_bytes));
  TEST_ASSERT_TRUE(store.write_default(key, template_bytes));
  EncodedProfile saved{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_default(key, saved)));
  TEST_ASSERT_EQUAL_UINT32(template_bytes.size, saved.size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(template_bytes.bytes.data(), saved.bytes.data(), template_bytes.size);
  Hx711Profile newer = unresolved; newer.manifest.profile_version = 2U;
  EncodedProfile newer_bytes{}; TEST_ASSERT_TRUE(codec.encode(newer, newer_bytes));
  TEST_ASSERT_FALSE(store.write_default(key, newer_bytes));
  EncodedProfile after_failed_overwrite{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(store.read_default(key, after_failed_overwrite)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(template_bytes.bytes.data(), after_failed_overwrite.bytes.data(), template_bytes.size);

  ProfilePersistence active(store);
  Hx711Profile calibrated = hx(3U); EncodedProfile active_bytes{};
  TEST_ASSERT_TRUE(codec.encode(calibrated, active_bytes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed), static_cast<int>(active.commit(key, active_bytes)));
  ProfileActivationMetadata before{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_activation(key, before)));
  EncodedProfile slot_a_before{}, slot_b_before{};
  const ProfileReadStatus a_status_before = store.read_slot(key, ProfileSlot::A, slot_a_before);
  const ProfileReadStatus b_status_before = store.read_slot(key, ProfileSlot::B, slot_b_before);
  EncodedProfile hx_template{};
  unresolved.manifest.profile_version = 4U;
  TEST_ASSERT_TRUE(codec.encode(unresolved, hx_template));
  TEST_ASSERT_TRUE(codec.validate_template(hx_template, key));
  // Defaults are absent-only even after a separate active-profile commit.
  TEST_ASSERT_FALSE(store.write_default(key, hx_template));
  EncodedProfile hx_template_readback{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_default(key, hx_template_readback)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(template_bytes.bytes.data(), hx_template_readback.bytes.data(), template_bytes.size);
  ProfileActivationMetadata after{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_activation(key, after)));
  TEST_ASSERT_EQUAL_UINT32(before.generation, after.generation);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(before.active_hash.data(), after.active_hash.data(), before.active_hash.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(before.active_slot), static_cast<int>(after.active_slot));
  EncodedProfile slot_a_after{}, slot_b_after{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(a_status_before), static_cast<int>(store.read_slot(key, ProfileSlot::A, slot_a_after)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(b_status_before), static_cast<int>(store.read_slot(key, ProfileSlot::B, slot_b_after)));
  if (a_status_before == ProfileReadStatus::Found) TEST_ASSERT_EQUAL_UINT8_ARRAY(slot_a_before.bytes.data(), slot_a_after.bytes.data(), slot_a_before.size);
  if (b_status_before == ProfileReadStatus::Found) TEST_ASSERT_EQUAL_UINT8_ARRAY(slot_b_before.bytes.data(), slot_b_after.bytes.data(), slot_b_before.size);
}

void test_default_template_store_faults_and_invalid_bytes() {
  MemoryProfileStore store{}; ProfileCodec codec{};
  const ProfileKey key{ProfileKind::Calibration, ProfileTarget::Hx711Organic};
  Hx711Profile unresolved{}; manifest<int>(unresolved.manifest, key.target, 1U);
  unresolved.manifest.validity = ProfileValidity::Invalid;
  EncodedProfile encoded{}; TEST_ASSERT_TRUE(codec.encode(unresolved, encoded));
  EncodedProfile damaged = encoded; damaged.bytes[damaged.size - 1U] ^= 1U;
  TEST_ASSERT_FALSE(codec.validate_template(damaged, key));
  damaged = encoded; damaged.bytes[8] ^= 1U; TEST_ASSERT_FALSE(codec.validate_template(damaged, key));
  TEST_ASSERT_FALSE(codec.validate_template(encoded, ProfileKey{ProfileKind::Calibration, ProfileTarget::Hx711B3}));
  Hx711Profile nonfinite = unresolved; nonfinite.counts_per_kg = NAN;
  EncodedProfile invalid_float{}; TEST_ASSERT_TRUE(codec.encode(nonfinite, invalid_float));
  TEST_ASSERT_FALSE(codec.validate_template(invalid_float, key));
  damaged = encoded; damaged.size = damaged.bytes.size() + 1U;
  TEST_ASSERT_FALSE(codec.validate_template(damaged, key));
  OperationalCapacityProfile invalid_enum{};
  manifest<int>(invalid_enum.manifest, ProfileTarget::Hx711Organic, 1U);
  invalid_enum.manifest.kind = ProfileKind::Capacity;
  invalid_enum.stream = static_cast<StreamId>(255U);
  EncodedProfile invalid_stream{}; TEST_ASSERT_TRUE(codec.encode(invalid_enum, invalid_stream));
  TEST_ASSERT_FALSE(codec.validate_template(invalid_stream, ProfileKey{ProfileKind::Capacity, ProfileTarget::Hx711Organic}));
  store.fail_next(MemoryProfileStore::Fault::WriteDefault);
  TEST_ASSERT_FALSE(store.write_default(key, encoded));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound), static_cast<int>(store.read_default(key, damaged)));
  store.fail_next(MemoryProfileStore::Fault::CorruptAfterWrite);
  TEST_ASSERT_FALSE(store.write_default(key, encoded));
  EncodedProfile corrupt_default{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Invalid), static_cast<int>(store.read_default(key, corrupt_default)));
  EncodedProfile corrupted_bytes = corrupt_default;
  TEST_ASSERT_FALSE(store.write_default(key, encoded));
  EncodedProfile still_corrupt{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Invalid), static_cast<int>(store.read_default(key, still_corrupt)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(corrupted_bytes.bytes.data(), still_corrupt.bytes.data(), corrupted_bytes.size);
  // A corrupt occupied default must not be overwritten to arrange another
  // fault scenario. Use a fresh store for failure after a legitimate write.
  MemoryProfileStore readback_store{};
  readback_store.fail_next(MemoryProfileStore::Fault::ReadDefaultback);
  TEST_ASSERT_FALSE(readback_store.write_default(key, encoded));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(readback_store.read_default(key, damaged)));
  readback_store.fail_next(MemoryProfileStore::Fault::ReadDefault);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Error), static_cast<int>(readback_store.read_default(key, damaged)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Error), static_cast<int>(NvsProfileStore{}.read_default(key, damaged)));
  TEST_ASSERT_FALSE(NvsProfileStore{}.write_default(key, encoded));
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
  EncodedProfile noncanonical_ds = encoded;
  constexpr std::size_t kDsHasRomOffset = kProfileHeaderBytes;
  noncanonical_ds.bytes[kDsHasRomOffset] = 2U;
  const auto ds_digest = sigap::telemetry_contract::sha256(std::string_view(
      reinterpret_cast<const char*>(noncanonical_ds.bytes.data()),
      noncanonical_ds.size - kProfileHashBytes));
  std::copy(ds_digest.begin(), ds_digest.end(),
            noncanonical_ds.bytes.begin() + noncanonical_ds.size - kProfileHashBytes);
  TEST_ASSERT_FALSE(codec.decode(noncanonical_ds, ds_restored));
  TEST_ASSERT_FALSE(codec.validate_template(noncanonical_ds,
      ProfileKey{ProfileKind::Calibration, ProfileTarget::Ds18b20B3}));

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

  OperationalCapacityProfile capacity{};
  manifest<int>(capacity.manifest, ProfileTarget::Hx711Organic, 7U);
  capacity.manifest.kind = ProfileKind::Capacity;
  capacity.stream = StreamId::Organic;
  capacity.operational_capacity_kg = 4.0F;
  capacity.present = true;
  TEST_ASSERT_TRUE(codec.encode(capacity, encoded));
  OperationalCapacityProfile capacity_restored{};
  TEST_ASSERT_TRUE(codec.decode(encoded, capacity_restored));
  TEST_ASSERT_TRUE(capacity_restored.valid_for_normalization());
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 4.0F,
                           capacity_restored.operational_capacity_kg);
  TEST_ASSERT_TRUE(codec.validate(
      encoded, ProfileKey{ProfileKind::Capacity, ProfileTarget::Hx711Organic}));
}

void test_profile_provenance_is_part_of_hashed_bytes() {
  ProfileCodec codec{};
  Hx711Profile first = hx(1U);
  first.manifest.provenance.present = true;
  first.manifest.provenance.monotonic_us = 100U;
  Hx711Profile second = first;
  second.manifest.provenance.monotonic_us = 101U;
  EncodedProfile first_encoded{};
  EncodedProfile second_encoded{};
  TEST_ASSERT_TRUE(codec.encode(first, first_encoded));
  TEST_ASSERT_TRUE(codec.encode(second, second_encoded));
  TEST_ASSERT_NOT_EQUAL(0, std::memcmp(first_encoded.hash().data(),
                                       second_encoded.hash().data(),
                                       first_encoded.hash().size()));
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
  // Gas profiles retain explicit electrical/reference provenance rather than
  // overloading legacy fields. Capacity growth is bounded and target-build RAM
  // usage is verified separately; legacy encoded lengths/hashes stay unchanged.
  TEST_ASSERT_TRUE(sizeof(EncodedProfile) <= 448U);
  TEST_ASSERT_TRUE(sizeof(ProfileManager) <= 20000U);
}

void test_mq7_adc_and_heater_profiles_coexist_by_typed_key() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  ProfileManager manager(persistence, codec);
  TargetProfileSet runtime{};
  manager.bind_runtime_profiles(runtime);
  const ProfileKey adc_key{ProfileKind::Adc, ProfileTarget::Mq7};
  const ProfileKey heater_key{ProfileKind::Heater, ProfileTarget::Mq7};
  TEST_ASSERT_TRUE(manager.load(adc_key));
  TEST_ASSERT_TRUE(manager.load(heater_key));

  MqAdcProfile adc{};
  manifest<int>(adc.manifest, ProfileTarget::Mq7, 3U);
  adc.reference_mv = 3300U;
  adc.full_scale_counts = 4095U;
  adc.divider_numerator = 2U;
  adc.divider_denominator = 1U;
  EncodedProfile adc_encoded{};
  TEST_ASSERT_TRUE(codec.encode(adc, adc_encoded));
  TEST_ASSERT_TRUE(manager.stage_import(adc_key, adc_encoded));
  TEST_ASSERT_TRUE(manager.preview(adc_key));
  TEST_ASSERT_TRUE(manager.commit(adc_key));

  Mq7HeaterProfile heater{};
  heater.present = true;
  heater.schema_version = 1U;
  heater.profile_version = 7U;
  heater.target_id = static_cast<uint32_t>(ProfileTarget::Mq7);
  heater.preheat_us = 10U;
  heater.high_clean_us = 20U;
  heater.low_detect_us = 30U;
  EncodedProfile heater_encoded{};
  TEST_ASSERT_TRUE(codec.encode(heater, heater_encoded));
  TEST_ASSERT_TRUE(manager.stage_import(heater_key, heater_encoded));
  TEST_ASSERT_TRUE(manager.preview(heater_key));
  TEST_ASSERT_TRUE(manager.commit(heater_key));

  TEST_ASSERT_NOT_NULL(runtime.mq_adc[2]);
  TEST_ASSERT_NOT_NULL(runtime.heater);
  TEST_ASSERT_EQUAL_UINT32(3U, runtime.mq_adc[2]->manifest.profile_version);
  TEST_ASSERT_EQUAL_UINT32(7U, runtime.heater->profile_version);
}

void test_capacity_profile_has_independent_lifecycle_and_runtime_binding() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  ProfileManager manager(persistence, codec);
  TargetProfileSet runtime{};
  manager.bind_runtime_profiles(runtime);
  const ProfileKey key{ProfileKind::Capacity, ProfileTarget::Hx711Organic};
  TEST_ASSERT_TRUE(manager.load(key));

  OperationalCapacityProfile capacity{};
  manifest<int>(capacity.manifest, ProfileTarget::Hx711Organic, 1U);
  capacity.manifest.kind = ProfileKind::Capacity;
  capacity.stream = StreamId::Organic;
  capacity.operational_capacity_kg = 4.0F;
  capacity.present = true;
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(capacity, encoded));
  TEST_ASSERT_TRUE(manager.stage_import(key, encoded));
  TEST_ASSERT_TRUE(manager.validate(key));
  TEST_ASSERT_TRUE(manager.preview(key));
  TEST_ASSERT_TRUE(manager.commit(key));
  TEST_ASSERT_NOT_NULL(runtime.capacity[0]);
  TEST_ASSERT_TRUE(runtime.capacity[0]->valid_for_normalization());
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 4.0F,
                           runtime.capacity[0]->operational_capacity_kg);

  ProfileManager reloaded(persistence, codec);
  TargetProfileSet reloaded_runtime{};
  reloaded.bind_runtime_profiles(reloaded_runtime);
  TEST_ASSERT_TRUE(reloaded.load(key));
  TEST_ASSERT_TRUE(reloaded_runtime.capacity[0]->valid_for_normalization());
  TEST_ASSERT_EQUAL_UINT32(1U,
                           reloaded_runtime.capacity[0]->manifest.profile_version);
}

void test_corrupt_activation_metadata_never_promotes_staged_slot() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  EncodedProfile v1{};
  EncodedProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(hx(1U), v1));
  TEST_ASSERT_TRUE(codec.encode(hx(2U), v2));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v1)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v2)));
  store.corrupt_activation(default_profile_key(ProfileTarget::Hx711Organic));
  EncodedProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Invalid),
                        static_cast<int>(persistence.load(ProfileTarget::Hx711Organic, loaded)));
}

void test_corrupt_newest_metadata_copy_uses_previous_committed_generation() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  EncodedProfile v1{};
  EncodedProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(hx(1U), v1));
  TEST_ASSERT_TRUE(codec.encode(hx(2U), v2));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v1)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic, v2)));
  store.corrupt_activation_copy(default_profile_key(ProfileTarget::Hx711Organic), 1U);
  EncodedProfile loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(persistence.load(ProfileTarget::Hx711Organic, loaded)));
  Hx711Profile restored{};
  TEST_ASSERT_TRUE(codec.decode(loaded, restored));
  TEST_ASSERT_EQUAL_UINT32(1U, restored.manifest.profile_version);
}

void test_profile_audit_ring_records_activation_context_and_overflow() {
  MemoryProfileStore store{};
  ProfilePersistence persistence(store);
  ProfileCodec codec{};
  ProfileAuditRing<2U> audit{};
  ProfileManager manager(persistence, codec, &audit);
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 9U;
  manager.set_audit_context(12345U, config_ref);
  TEST_ASSERT_TRUE(manager.load(ProfileTarget::Hx711Organic));
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(hx(1U), encoded));
  TEST_ASSERT_TRUE(manager.stage_import(ProfileTarget::Hx711Organic, encoded));
  TEST_ASSERT_TRUE(manager.preview(ProfileTarget::Hx711Organic));
  TEST_ASSERT_TRUE(manager.commit(ProfileTarget::Hx711Organic));

  AuditEvent event{};
  TEST_ASSERT_TRUE(audit.try_pop(event));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AuditEventKind::ProfileActivated),
                        static_cast<int>(event.kind));
  TEST_ASSERT_EQUAL_UINT64(12345U, event.monotonic_us);
  TEST_ASSERT_TRUE(event.config_ref.present);
  TEST_ASSERT_EQUAL_UINT64(9U, event.config_ref.config_version);
  TEST_ASSERT_TRUE(event.current.present);
  TEST_ASSERT_EQUAL_UINT32(1U, event.current.profile_version);

  audit.config_committed(config_ref);
  audit.config_committed(config_ref);
  audit.config_committed(config_ref);
  TEST_ASSERT_EQUAL_UINT32(1U, audit.overflow_count());
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_default_template_store_is_absent_only_and_never_active_valid);
  RUN_TEST(test_default_template_store_faults_and_invalid_bytes);
  RUN_TEST(test_profile_codec_round_trips_all_profile_families);
  RUN_TEST(test_profile_provenance_is_part_of_hashed_bytes);
  RUN_TEST(test_profile_commit_pointer_last_and_reload);
  RUN_TEST(test_failed_commit_keeps_previous_active);
  RUN_TEST(test_readback_and_pointer_faults_keep_previous_generation);
  RUN_TEST(test_target_mismatch_hash_schema_and_reset_are_rejected);
  RUN_TEST(test_profile_manager_import_preview_commit_and_snapshot_identity);
  RUN_TEST(test_profile_memory_footprint_is_bounded);
  RUN_TEST(test_mq7_adc_and_heater_profiles_coexist_by_typed_key);
  RUN_TEST(test_capacity_profile_has_independent_lifecycle_and_runtime_binding);
  RUN_TEST(test_corrupt_activation_metadata_never_promotes_staged_slot);
  RUN_TEST(test_corrupt_newest_metadata_copy_uses_previous_committed_generation);
  RUN_TEST(test_profile_audit_ring_records_activation_context_and_overflow);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
