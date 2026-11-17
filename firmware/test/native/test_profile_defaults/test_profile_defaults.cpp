#include <cstdint>
#include <cstring>

#include <unity.h>

#include "acquisition/default_profiles.h"
#include "acquisition/profile_manager.h"
#include "acquisition/profile_store.h"

namespace {
using namespace sigap::acquisition;

bool same(const EncodedProfile& a, const EncodedProfile& b) {
  return a.size == b.size && a.bytes == b.bytes && a.hash() == b.hash();
}

void test_reference_targets_are_exact_four_and_graph_has_thirteen() {
  const auto physical = enabled_default_targets(true);
  const ProfileTarget expected[] = {ProfileTarget::Hx711Organic,
      ProfileTarget::UltrasonicOrganic, ProfileTarget::Mq135, ProfileTarget::Sht31_44};
  std::size_t count = 0;
  for (const auto target : physical) if (target != ProfileTarget::Unknown) {
    TEST_ASSERT_TRUE(count < 4U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected[count]), static_cast<int>(target));
    ++count;
  }
  TEST_ASSERT_EQUAL_UINT32(4U, count);
  const ProfileTarget full[] = {ProfileTarget::Hx711Organic, ProfileTarget::Hx711Inorganic, ProfileTarget::Hx711B3, ProfileTarget::UltrasonicOrganic, ProfileTarget::UltrasonicInorganic, ProfileTarget::UltrasonicB3, ProfileTarget::Mq4, ProfileTarget::Mq135, ProfileTarget::Mq7, ProfileTarget::Mq8, ProfileTarget::Ds18b20B3, ProfileTarget::Sht31_44, ProfileTarget::Sht31_45};
  count = 0;
  for (const auto target : enabled_default_targets(false)) if (target != ProfileTarget::Unknown) { TEST_ASSERT_TRUE(count < 13U); TEST_ASSERT_EQUAL_INT(static_cast<int>(full[count]), static_cast<int>(target)); ++count; }
  TEST_ASSERT_EQUAL_UINT32(13U, count);
  for (std::size_t i = 0; i < count; ++i) for (std::size_t j = i + 1; j < count; ++j) TEST_ASSERT_TRUE(full[i] != full[j]);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileKind::Adc), static_cast<int>(default_profile_key(ProfileTarget::Mq4).kind));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileKind::Calibration), static_cast<int>(default_profile_key(ProfileTarget::Hx711Organic).kind));
}

void test_bootstrap_initializes_sht_and_stores_only_three_templates() {
  MemoryProfileStore store{}; ProfileCodec codec{}; ProfilePersistence persistence(store);
  ProfileManager manager(persistence, codec); DefaultProfiles defaults(manager, store, codec);
  const auto result = defaults.bootstrap_enabled(true);
  TEST_ASSERT_EQUAL_UINT32(1U, result.initialized);
  TEST_ASSERT_EQUAL_UINT32(3U, result.stored);
  TEST_ASSERT_EQUAL_UINT32(0U, result.errors);
  TEST_ASSERT_EQUAL_UINT32(0U, result.blocked);
  EncodedProfile sht{};
  const ProfileKey sht_key{ProfileKind::Calibration, ProfileTarget::Sht31_44};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(persistence.load(sht_key, sht)));
  TEST_ASSERT_TRUE(codec.validate(sht, sht_key));
  Sht31Profile decoded{}; TEST_ASSERT_TRUE(codec.decode(sht, decoded));
  TEST_ASSERT_EQUAL_UINT8(0x44U, decoded.address);
  TEST_ASSERT_EQUAL_INT(0, decoded.temperature_offset_centi_c);
  TEST_ASSERT_EQUAL_INT(0, decoded.humidity_offset_centi_pct);
  TEST_ASSERT_TRUE(decoded.manifest.provenance.present);
  TEST_ASSERT_EQUAL_MEMORY("FACTORY_DEFAULT", decoded.manifest.provenance.reference_note.data(), 15U);
  for (ProfileTarget target : {ProfileTarget::Hx711Organic, ProfileTarget::UltrasonicOrganic, ProfileTarget::Mq135}) {
    EncodedProfile saved{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_default(default_profile_key(target), saved)));
    TEST_ASSERT_TRUE(codec.validate_template(saved, default_profile_key(target)));
    TEST_ASSERT_FALSE(codec.validate(saved, default_profile_key(target)));
  }
  for (ProfileTarget absent : {ProfileTarget::Mq4, ProfileTarget::Mq7, ProfileTarget::Unknown}) {
    EncodedProfile ignored{};
    if (absent != ProfileTarget::Unknown)
      TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound), static_cast<int>(store.read_default(default_profile_key(absent), ignored)));
  }
  EncodedProfile unknown{};
  TEST_ASSERT_FALSE(defaults.build(ProfileTarget::Unknown, unknown));
  TEST_ASSERT_FALSE(codec.validate_template(unknown, default_profile_key(ProfileTarget::Unknown)));
}

void test_rebootstrap_preserves_templates_and_active_factory_bytes() {
  MemoryProfileStore store{}; ProfileCodec codec{}; ProfilePersistence persistence(store);
  ProfileManager first(persistence, codec); DefaultProfiles bootstrap(first, store, codec);
  bootstrap.bootstrap_enabled(true);
  EncodedProfile hxtemplate{}, sht{};
  store.read_default(default_profile_key(ProfileTarget::Hx711Organic), hxtemplate);
  persistence.load(default_profile_key(ProfileTarget::Sht31_44), sht);
  ProfileManager second(persistence, codec); DefaultProfiles again(second, store, codec);
  const auto result = again.bootstrap_enabled(true);
  TEST_ASSERT_EQUAL_UINT32(4U, result.preserved);
  EncodedProfile got_template{}, got_sht{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_default(default_profile_key(ProfileTarget::Hx711Organic), got_template)));
  TEST_ASSERT_TRUE(same(hxtemplate, got_template));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(persistence.load(default_profile_key(ProfileTarget::Sht31_44), got_sht)));
  TEST_ASSERT_TRUE(same(sht, got_sht));
}

void test_saved_calibration_survives_bootstrap_and_real_commit_wins() {
  MemoryProfileStore store{}; ProfileCodec codec{}; ProfilePersistence persistence(store);
  const auto key = default_profile_key(ProfileTarget::Hx711Organic);
  Hx711Profile saved{}; saved.manifest.kind = ProfileKind::Calibration; saved.manifest.schema_version = 1U;
  saved.manifest.profile_version = 8U; saved.manifest.target_id = static_cast<uint32_t>(key.target);
  saved.manifest.validity = ProfileValidity::Valid; saved.counts_per_kg = 123.0F;
  saved.tare_offset_counts = 42;
  EncodedProfile saved_bytes{}; TEST_ASSERT_TRUE(codec.encode(saved, saved_bytes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed), static_cast<int>(persistence.commit(key, saved_bytes)));
  ProfileManager manager(persistence, codec); DefaultProfiles defaults(manager, store, codec);
  defaults.bootstrap_enabled(true);
  EncodedProfile after{}; TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(persistence.load(key, after)));
  TEST_ASSERT_TRUE(same(saved_bytes, after));
  Hx711Profile newer = saved; newer.manifest.profile_version = 9U; newer.counts_per_kg = 456.0F;
  EncodedProfile newer_bytes{}; TEST_ASSERT_TRUE(codec.encode(newer, newer_bytes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed), static_cast<int>(persistence.commit(key, newer_bytes)));
  ProfileManager loaded(persistence, codec); TEST_ASSERT_TRUE(loaded.load(key));
  ActiveProfileSnapshot snapshot{}; TEST_ASSERT_TRUE(loaded.snapshot(key, snapshot));
  TEST_ASSERT_TRUE(same(newer_bytes, snapshot.encoded));
}

void test_invalid_default_and_read_error_block_without_rewrite() {
  MemoryProfileStore store{}; ProfileCodec codec{}; ProfilePersistence persistence(store);
  ProfileManager manager(persistence, codec); DefaultProfiles defaults(manager, store, codec);
  const ProfileTarget target = ProfileTarget::Hx711Organic;
  const ProfileKey key = default_profile_key(target);
  EncodedProfile candidate{}; TEST_ASSERT_TRUE(defaults.build(target, candidate));
  store.fail_next(MemoryProfileStore::Fault::CorruptAfterWrite);
  TEST_ASSERT_FALSE(store.write_default(key, candidate));
  EncodedProfile bad{}; TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Invalid), static_cast<int>(store.read_default(key, bad)));
  EncodedProfile original = bad;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DefaultOutcome::InvalidTemplate), static_cast<int>(defaults.bootstrap(target)));
  EncodedProfile reread{}; TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Invalid), static_cast<int>(store.read_default(key, reread)));
  TEST_ASSERT_TRUE(same(original, reread));
  MemoryProfileStore error_store{};
  error_store.fail_next(MemoryProfileStore::Fault::ReadDefault);
  // Store errors are observable and do not silently create a replacement.
  class ErrorStore final : public IProfileSlotStore {
   public:
    explicit ErrorStore(MemoryProfileStore& inner) : inner_(inner) {}
    ProfileReadStatus read_activation(ProfileTarget t, ProfileActivationMetadata& m) const noexcept override { return inner_.read_activation(t,m); }
    ProfileReadStatus read_slot(ProfileTarget t, ProfileSlot s, EncodedProfile& e) const noexcept override { return inner_.read_slot(t,s,e); }
    bool write_slot(ProfileTarget t, ProfileSlot s, const EncodedProfile& e) noexcept override { return inner_.write_slot(t,s,e); }
    bool write_activation(ProfileTarget t, const ProfileActivationMetadata& m) noexcept override { return inner_.write_activation(t,m); }
    ProfileReadStatus read_activation(ProfileKey k, ProfileActivationMetadata& m) const noexcept override { return inner_.read_activation(k,m); }
    ProfileReadStatus read_slot(ProfileKey k, ProfileSlot s, EncodedProfile& e) const noexcept override { return inner_.read_slot(k,s,e); }
    bool write_slot(ProfileKey k, ProfileSlot s, const EncodedProfile& e) noexcept override { return inner_.write_slot(k,s,e); }
    bool write_activation(ProfileKey k, const ProfileActivationMetadata& m) noexcept override { return inner_.write_activation(k,m); }
    ProfileReadStatus read_default(ProfileKey k, EncodedProfile& e) const noexcept override { return inner_.read_default(k,e); }
    bool write_default(ProfileKey k, const EncodedProfile& e) noexcept override { ++writes; return inner_.write_default(k,e); }
    mutable unsigned writes{0};
   private: MemoryProfileStore& inner_;
  } error_store_wrapper(error_store);
  ProfilePersistence ep(error_store_wrapper); ProfileManager em(ep, codec); DefaultProfiles ed(em, error_store_wrapper, codec);
  const ProfileKey hx_key = default_profile_key(ProfileTarget::Hx711Organic);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DefaultOutcome::ReadError), static_cast<int>(ed.bootstrap(ProfileTarget::Hx711Organic)));
  TEST_ASSERT_EQUAL_UINT32(0U, error_store_wrapper.writes);
  EncodedProfile untouched{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound), static_cast<int>(error_store.read_default(hx_key, untouched)));
}

void test_newer_saved_template_is_preserved_byte_for_byte() {
  MemoryProfileStore store{}; ProfileCodec codec{}; ProfilePersistence persistence(store);
  ProfileManager manager(persistence, codec); DefaultProfiles defaults(manager, store, codec);
  const auto key = default_profile_key(ProfileTarget::Hx711Organic);
  EncodedProfile canonical{}; TEST_ASSERT_TRUE(defaults.build(ProfileTarget::Hx711Organic, canonical));
  Hx711Profile newer{}; TEST_ASSERT_TRUE(codec.decode(canonical, newer));
  newer.manifest.profile_version = 9U;
  EncodedProfile newer_bytes{}; TEST_ASSERT_TRUE(codec.encode(newer, newer_bytes));
  TEST_ASSERT_TRUE(store.write_default(key, newer_bytes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DefaultOutcome::Preserved), static_cast<int>(defaults.bootstrap(ProfileTarget::Hx711Organic)));
  EncodedProfile reread{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_default(key, reread)));
  TEST_ASSERT_TRUE(same(newer_bytes, reread));
  TEST_ASSERT_FALSE(same(canonical, reread));
  Hx711Profile decoded{}; TEST_ASSERT_TRUE(codec.decode(reread, decoded));
  TEST_ASSERT_EQUAL_UINT32(9U, decoded.manifest.profile_version);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileValidity::Invalid), static_cast<int>(decoded.manifest.validity));
}

void test_sht_bootstrap_preserves_all_nonpristine_storage_states() {
  const auto target = ProfileTarget::Sht31_44; const auto key = default_profile_key(target);
  ProfileCodec codec{}; EncodedProfile valid{};
  { MemoryProfileStore s{}; ProfilePersistence p(s, codec); ProfileManager m(p, codec); DefaultProfiles d(m, s, codec); TEST_ASSERT_TRUE(d.build(target, valid)); }
  for (int scenario = 0; scenario < 5; ++scenario) {
    MemoryProfileStore store{}; ProfilePersistence persistence(store, codec);
    ProfileManager manager(persistence, codec); DefaultProfiles defaults(manager, store, codec);
    if (scenario == 0) { TEST_ASSERT_TRUE(manager.stage_import(key, valid)); TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileManagerState::Draft), static_cast<int>(manager.state(key))); }
    if (scenario == 1) { TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed), static_cast<int>(persistence.commit(key, valid))); TEST_ASSERT_TRUE(persistence.reset(key)); }
    if (scenario == 2) TEST_ASSERT_TRUE(store.write_slot(target, ProfileSlot::A, valid));
    if (scenario == 3) { TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed), static_cast<int>(persistence.commit(key, valid))); ProfileActivationMetadata md{}; TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(store.read_activation(target, md))); store.corrupt_slot(target, md.active_slot); }
    EncodedProfile raw_before[2]{}; ProfileReadStatus slot_before[2];
    for (int i=0;i<2;++i) slot_before[i]=store.read_slot(target, i==0?ProfileSlot::A:ProfileSlot::B, raw_before[i]);
    ProfileActivationMetadata before{}; auto activation_before=store.read_activation(target,before);
    EncodedProfile default_before{}; auto default_status_before=store.read_default(key,default_before);
    if (scenario == 4) store.fail_next(MemoryProfileStore::Fault::ReadActivation);
    const auto outcome=defaults.bootstrap(target);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(scenario==0?DefaultOutcome::Busy:scenario==4?DefaultOutcome::ReadError:DefaultOutcome::Preserved),static_cast<int>(outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound),static_cast<int>(store.read_default(key,default_before)));
    for(int i=0;i<2;++i){ EncodedProfile after{}; TEST_ASSERT_EQUAL_INT(static_cast<int>(slot_before[i]),static_cast<int>(store.read_slot(target,i==0?ProfileSlot::A:ProfileSlot::B,after))); if(slot_before[i]==ProfileReadStatus::Found) TEST_ASSERT_TRUE(same(raw_before[i],after)); }
    ProfileActivationMetadata after{}; TEST_ASSERT_EQUAL_INT(static_cast<int>(activation_before),static_cast<int>(store.read_activation(target,after)));
    if(activation_before==ProfileReadStatus::Found){ TEST_ASSERT_EQUAL_UINT32(before.generation,after.generation); TEST_ASSERT_EQUAL_INT(before.has_active,after.has_active); TEST_ASSERT_EQUAL_INT(static_cast<int>(before.active_slot),static_cast<int>(after.active_slot)); TEST_ASSERT_EQUAL_UINT32(before.active_version,after.active_version); TEST_ASSERT_TRUE(before.active_hash==after.active_hash); TEST_ASSERT_EQUAL_INT(before.has_previous,after.has_previous); TEST_ASSERT_EQUAL_INT(static_cast<int>(before.previous_slot),static_cast<int>(after.previous_slot)); TEST_ASSERT_EQUAL_UINT32(before.previous_version,after.previous_version); TEST_ASSERT_TRUE(before.previous_hash==after.previous_hash); TEST_ASSERT_TRUE(before.integrity==after.integrity); }
    EncodedProfile default_after{}; auto default_status_after=store.read_default(key,default_after);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(default_status_before),static_cast<int>(default_status_after));
    if(default_status_before==ProfileReadStatus::NotFound) TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound),static_cast<int>(default_status_after));
    if(default_status_before==ProfileReadStatus::Found || default_status_before==ProfileReadStatus::Invalid) TEST_ASSERT_TRUE(same(default_before,default_after));
    if(scenario==0) TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileManagerState::Draft),static_cast<int>(manager.state(key)));
    if(scenario==3){ EncodedProfile damaged{}; ProfileActivationMetadata md{}; store.read_activation(target,md); TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),static_cast<int>(store.read_slot(target,md.active_slot,damaged))); TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfilePersistence::StorageInspection::Occupied),static_cast<int>(persistence.inspect_storage(key))); }
  }
}

void test_invalid_factory_candidate_cannot_preempt_saved_guard() {
  MemoryProfileStore store{}; ProfileCodec codec{}; ProfilePersistence persistence(store);
  ProfileManager manager(persistence, codec);
  const auto key = default_profile_key(ProfileTarget::Sht31_44);
  EncodedProfile active{};
  Sht31Profile saved{}; saved.manifest.kind = ProfileKind::Calibration; saved.manifest.schema_version = 1U;
  saved.manifest.profile_version = 5U; saved.manifest.target_id = static_cast<uint32_t>(key.target); saved.manifest.validity = ProfileValidity::Valid;
  saved.address = 0x44U;
  TEST_ASSERT_TRUE(codec.encode(saved, active));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed), static_cast<int>(persistence.commit(key, active)));
  EncodedProfile invalid{}; saved.manifest.profile_version = 99U; saved.manifest.validity = ProfileValidity::Invalid;
  TEST_ASSERT_TRUE(codec.encode(saved, invalid));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FactoryProfileInitResult::Preserved), static_cast<int>(manager.initialize_pristine(key, invalid)));
  EncodedProfile after{}; TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found), static_cast<int>(persistence.load(key, after)));
  TEST_ASSERT_TRUE(same(active, after));
}

}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_reference_targets_are_exact_four_and_graph_has_thirteen);
  RUN_TEST(test_bootstrap_initializes_sht_and_stores_only_three_templates);
  RUN_TEST(test_rebootstrap_preserves_templates_and_active_factory_bytes);
  RUN_TEST(test_saved_calibration_survives_bootstrap_and_real_commit_wins);
  RUN_TEST(test_invalid_default_and_read_error_block_without_rewrite);
  RUN_TEST(test_newer_saved_template_is_preserved_byte_for_byte);
  RUN_TEST(test_sht_bootstrap_preserves_all_nonpristine_storage_states);
  RUN_TEST(test_invalid_factory_candidate_cannot_preempt_saved_guard);
  return UNITY_END();
}
