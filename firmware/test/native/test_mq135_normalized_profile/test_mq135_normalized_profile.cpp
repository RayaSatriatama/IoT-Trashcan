// S2 typed normalized MQ-135 profile: exact released byte layout, round-trip
// stability and retained legacy payload-v1/ADC canonical bytes.

#include <unity.h>

#include <cmath>
#include <cstring>
#include <string>

#include "acquisition/mq135_gas.h"
#include "acquisition/mq135_normalized.h"
#include "acquisition/profile_store.h"

using namespace sigap::acquisition;

void setUp() {}
void tearDown() {}

namespace {

ProfileManifest manifest(uint32_t version, ProfileValidity validity) {
  ProfileManifest m{};
  m.kind = ProfileKind::Calibration;
  m.schema_version = kProfileSchemaVersion;
  m.profile_version = version;
  m.target_id = static_cast<uint32_t>(ProfileTarget::Mq135);
  m.validity = validity;
  m.provenance.present = true;
  m.provenance.boot_id.present = true;
  m.provenance.boot_id.bytes.fill(0x11);
  m.provenance.monotonic_us = 9000000ULL;
  m.provenance.recorded_at.available = true;
  m.provenance.recorded_at.epoch_ms = 1763000000000LL;
  m.provenance.time_quality = TimeQuality::Synchronized;
  m.provenance.reference_primary = 2500;
  m.provenance.reference_secondary = 5000;
  m.provenance.evidence_count = 5U;
  m.provenance.evidence_sequence = 42U;
  m.provenance.units = 6U;
  const char note[] = "normalized-evidence";
  std::memcpy(m.provenance.reference_note.data(), note, sizeof(note) - 1U);
  return m;
}

// Clean-air confirmed baseline: operator-declared initial preheat > 48 h.
Mq135NormalizedProfile clean_air_profile() {
  Mq135NormalizedProfile p{};
  p.manifest = manifest(3U, ProfileValidity::TestOnly);
  p.electrical.circuit_mv = 5000U;
  p.electrical.divider_top_ohm = 10000U;
  p.electrical.divider_bottom_ohm = 15000U;
  p.electrical.topology = Mq135Topology::DirectLoaded;
  p.electrical.hardware_confirmed = true;
  p.adc_hash.fill(0x2AU);
  p.adc_version = 4U;
  p.q0 = 1.25F;
  p.minimum_q = 1.20F;
  p.maximum_q = 1.30F;
  p.reference_pin_mv = 1500U;
  p.reference_ao_mv = 2500U;
  p.variation_percent = 1.5F;
  p.sample_count = 5U;
  p.rejected_count = 0U;
  p.first_us = 1000000ULL;
  p.last_us = 9000000ULL;
  p.evidence_hash.fill(0x3BU);
  p.criteria.samples = 5U;
  p.criteria.min_duration_us = 2000000ULL;
  p.criteria.max_age_us = 5000000ULL;
  p.criteria.max_variation_percent = 5.0F;
  const char identity[] = "SYNTHETIC-BOARD";
  std::memcpy(p.hardware_identity.data(), identity, sizeof(identity) - 1U);
  const char environment[] = "SYNTHETIC-CLEAN-AIR";
  std::memcpy(p.reference_environment.data(), environment, sizeof(environment) - 1U);
  p.temperature_present = true;
  p.temperature_centi_c = 2537;
  p.humidity_present = true;
  p.humidity_centi_pct = 4100U;
  p.heating_duration_us = kMq135MinimumInitialPreheatUs + 3600000000ULL;
  p.preparation_confirmed = true;
  p.preparation_method = Mq135PreparationMethod::OperatorDeclared;
  p.reference_kind = Mq135ReferenceKind::CleanAirConfirmed;
  p.adc_method = static_cast<uint8_t>(MqAdcCalibrationMethod::EspAdcCalEfuseTpFit);
  p.resolution_bits = 12U;
  p.attenuation_db = 11U;
  p.adc_unit = 1U;
  p.adc_channel = 4U;
  p.curve_id = kMq135Nh3CurveId;
  p.curve_version = kMq135Nh3CurveVersion;
  p.curve_source_sha256 = kMq135Nh3CurveSourceSha256;
  p.estimation_method = Mq135EstimationMethod::PiecewiseLogLog;
  return p;
}

uint16_t le16(const EncodedProfile& e, std::size_t o) {
  return static_cast<uint16_t>(e.bytes[o]) |
         static_cast<uint16_t>(static_cast<uint16_t>(e.bytes[o + 1U]) << 8U);
}
uint32_t le32(const EncodedProfile& e, std::size_t o) {
  uint32_t v{0U};
  for (unsigned i = 0; i < 4U; ++i) v |= static_cast<uint32_t>(e.bytes[o + i]) << (8U * i);
  return v;
}
uint64_t le64(const EncodedProfile& e, std::size_t o) {
  uint64_t v{0U};
  for (unsigned i = 0; i < 8U; ++i) v |= static_cast<uint64_t>(e.bytes[o + i]) << (8U * i);
  return v;
}
float lef32(const EncodedProfile& e, std::size_t o) {
  const uint32_t bits = le32(e, o);
  float v{0.0F};
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

// Legacy payload-v1 gas profile, encoded with the untouched legacy codec.
Mq135GasProfile legacy_profile() {
  Mq135GasProfile p{};
  p.manifest = manifest(1U, ProfileValidity::TestOnly);
  p.electrical.circuit_mv = 5000.0F;
  p.electrical.module_load_ohm = 10000.0F;
  p.electrical.divider_top_ohm = 10000.0F;
  p.electrical.divider_bottom_ohm = 15000.0F;
  p.electrical.topology = Mq135Topology::DirectLoaded;
  p.electrical.verified = true;
  p.adc_hash.fill(0x5AU);
  p.adc_version = 2U;
  p.r0_ohm = 50000.0F / 7.0F;
  p.reference.ready = true;
  p.reference.count = 3U;
  p.reference.rejected = 0U;
  p.reference.median_ohm = p.r0_ohm;
  p.reference.minimum_ohm = p.r0_ohm;
  p.reference.maximum_ohm = p.r0_ohm;
  p.reference.variation_percent = 0.0F;
  p.reference.first_us = 1000U;
  p.reference.last_us = 3000U;
  p.reference.evidence_hash.fill(0x6BU);
  p.criteria = {3U, 2000U, 500U, 10.0F};
  const char identity[] = "SYNTHETIC";
  std::memcpy(p.hardware_identity.data(), identity, sizeof(identity) - 1U);
  const char environment[] = "SYNTHETIC";
  std::memcpy(p.reference_environment.data(), environment, sizeof(environment) - 1U);
  p.temperature_centi_c = 2500;
  p.humidity_centi_percent = 5000U;
  p.heating_duration_us = 10000U;
  p.preparation_confirmed = true;
  p.manifest.provenance.evidence_count = 3U;
  return p;
}

}  // namespace

// Milestone oracle: exact released 373-byte payload inside a 427-byte envelope,
// every released offset honoured, and a stable decode/encode round-trip.
void test_normalized_profile_envelope_is_exactly_373_payload_427_envelope() {
  const auto profile = clean_air_profile();
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(profile, encoded));
  TEST_ASSERT_EQUAL_UINT32(427U, static_cast<unsigned>(encoded.size));
  TEST_ASSERT_EQUAL_UINT32(22U, static_cast<unsigned>(kProfileHeaderBytes));
  TEST_ASSERT_EQUAL_UINT16(373U, le16(encoded, 20U));
  TEST_ASSERT_EQUAL_HEX32(kProfileMagic, le32(encoded, 0U));
  TEST_ASSERT_EQUAL_UINT8(2U, encoded.bytes[22U]);  // payload discriminator
  TEST_ASSERT_TRUE(mq135_normalized_payload(encoded));

  // Spot-check the released layout offsets rather than trusting field order.
  TEST_ASSERT_EQUAL_UINT32(5000U, le32(encoded, 22U + 1U));
  TEST_ASSERT_EQUAL_UINT32(10000U, le32(encoded, 22U + 5U));
  TEST_ASSERT_EQUAL_UINT32(15000U, le32(encoded, 22U + 9U));
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 13U]);
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 14U]);
  TEST_ASSERT_EQUAL_MEMORY(profile.adc_hash.data(), encoded.bytes.data() + 22U + 15U, 32U);
  TEST_ASSERT_EQUAL_UINT32(4U, le32(encoded, 22U + 47U));
  TEST_ASSERT_EQUAL_FLOAT(1.25F, lef32(encoded, 22U + 51U));
  TEST_ASSERT_EQUAL_FLOAT(1.20F, lef32(encoded, 22U + 55U));
  TEST_ASSERT_EQUAL_FLOAT(1.30F, lef32(encoded, 22U + 59U));
  TEST_ASSERT_EQUAL_UINT32(1500U, le32(encoded, 22U + 63U));
  TEST_ASSERT_EQUAL_UINT32(2500U, le32(encoded, 22U + 67U));
  TEST_ASSERT_EQUAL_UINT16(5U, le16(encoded, 22U + 75U));
  TEST_ASSERT_EQUAL_UINT16(0U, le16(encoded, 22U + 77U));
  TEST_ASSERT_EQUAL_UINT64(1000000ULL, le64(encoded, 22U + 79U));
  TEST_ASSERT_EQUAL_UINT64(9000000ULL, le64(encoded, 22U + 87U));
  TEST_ASSERT_EQUAL_UINT16(5U, le16(encoded, 22U + 127U));
  TEST_ASSERT_EQUAL_UINT64(2000000ULL, le64(encoded, 22U + 129U));
  TEST_ASSERT_EQUAL_UINT64(5000000ULL, le64(encoded, 22U + 137U));
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 213U]);
  TEST_ASSERT_EQUAL_INT32(2537, static_cast<int32_t>(le32(encoded, 22U + 214U)));
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 218U]);
  TEST_ASSERT_EQUAL_UINT32(4100U, le32(encoded, 22U + 219U));
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 231U]);
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 232U]);
  TEST_ASSERT_EQUAL_UINT8(2U, encoded.bytes[22U + 233U]);
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 234U]);
  TEST_ASSERT_EQUAL_UINT8(12U, encoded.bytes[22U + 235U]);
  TEST_ASSERT_EQUAL_UINT8(11U, encoded.bytes[22U + 236U]);
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 237U]);
  TEST_ASSERT_EQUAL_UINT8(4U, encoded.bytes[22U + 238U]);
  TEST_ASSERT_EQUAL_UINT16(kMq135Nh3CurveId, le16(encoded, 22U + 239U));
  TEST_ASSERT_EQUAL_UINT16(kMq135Nh3CurveVersion, le16(encoded, 22U + 241U));
  TEST_ASSERT_EQUAL_MEMORY(kMq135Nh3CurveSourceSha256.data(),
                           encoded.bytes.data() + 22U + 243U, 32U);
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U + 275U]);
  // 276 + 97 = 373: provenance is the last released field.
  TEST_ASSERT_EQUAL_UINT32(373U, static_cast<unsigned>(276U + 97U));
}

void test_normalized_profile_round_trip_and_reencode_are_stable() {
  const auto profile = clean_air_profile();
  EncodedProfile first{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(profile, first));
  Mq135NormalizedProfile decoded{};
  TEST_ASSERT_TRUE(decode_mq135_normalized_profile(first, decoded));
  TEST_ASSERT_EQUAL_FLOAT(profile.q0, decoded.q0);
  TEST_ASSERT_EQUAL_FLOAT(profile.minimum_q, decoded.minimum_q);
  TEST_ASSERT_EQUAL_FLOAT(profile.maximum_q, decoded.maximum_q);
  TEST_ASSERT_EQUAL_UINT32(profile.electrical.circuit_mv, decoded.electrical.circuit_mv);
  TEST_ASSERT_EQUAL_UINT32(profile.adc_version, decoded.adc_version);
  TEST_ASSERT_EQUAL_UINT16(profile.sample_count, decoded.sample_count);
  TEST_ASSERT_EQUAL_UINT64(profile.heating_duration_us, decoded.heating_duration_us);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(profile.reference_kind),
                          static_cast<uint8_t>(decoded.reference_kind));
  TEST_ASSERT_EQUAL_MEMORY(profile.hardware_identity.data(),
                           decoded.hardware_identity.data(), 32U);
  TEST_ASSERT_TRUE(decoded.manifest.provenance.present);
  TEST_ASSERT_EQUAL_UINT32(profile.manifest.provenance.evidence_count,
                           decoded.manifest.provenance.evidence_count);
  TEST_ASSERT_EQUAL_UINT32(3U, decoded.manifest.profile_version);
  TEST_ASSERT_EQUAL_MEMORY(first.hash().data(), decoded.manifest.hash.data(), 32U);

  EncodedProfile second{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(decoded, second));
  TEST_ASSERT_EQUAL_UINT32(static_cast<unsigned>(first.size),
                           static_cast<unsigned>(second.size));
  TEST_ASSERT_EQUAL_MEMORY(first.bytes.data(), second.bytes.data(), first.size);
}

// Retained legacy bytes: the payload-v1 gas profile and the ADC profile still
// encode byte-for-byte identically and never pick up the normalized layout.
void test_legacy_payload_v1_and_adc_bytes_are_retained() {
  ProfileCodec codec{};
  const auto legacy = legacy_profile();
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(legacy, encoded));
  TEST_ASSERT_EQUAL_UINT8(1U, encoded.bytes[22U]);
  TEST_ASSERT_FALSE(mq135_normalized_payload(encoded));
  Mq135NormalizedProfile normalized{};
  TEST_ASSERT_FALSE(decode_mq135_normalized_profile(encoded, normalized));
  Mq135GasProfile decoded{};
  TEST_ASSERT_TRUE(codec.decode(encoded, decoded));
  TEST_ASSERT_EQUAL_FLOAT(legacy.r0_ohm, decoded.r0_ohm);
  TEST_ASSERT_EQUAL_UINT32(static_cast<unsigned>(kProfileHeaderBytes + 1U + 4U + 4U + 4U + 4U +
                                                 1U + 1U + 32U + 4U + 4U + 1U + 2U + 2U +
                                                 4U + 4U + 4U + 4U + 8U + 8U + 32U + 2U + 8U +
                                                 8U + 4U + 32U + 32U + 4U + 4U + 8U + 1U + 97U +
                                                 kProfileHashBytes),
                           static_cast<unsigned>(encoded.size));

  MqAdcProfile adc{};
  adc.manifest.kind = ProfileKind::Adc;
  adc.manifest.target_id = 8U;
  adc.manifest.profile_version = 1U;
  adc.manifest.validity = ProfileValidity::TestOnly;
  adc.manifest.provenance.present = true;
  adc.manifest.provenance.evidence_count = 3U;
  adc.reference_mv = 3000U;
  adc.full_scale_counts = 3000U;
  adc.divider_numerator = 3U;
  adc.divider_denominator = 5U;
  EncodedProfile adc_bytes{};
  TEST_ASSERT_TRUE(codec.encode(adc, adc_bytes));
  // The ADC payload has no discriminator byte and is never normalized.
  TEST_ASSERT_FALSE(mq135_normalized_payload(adc_bytes));
  MqAdcProfile adc_round{};
  TEST_ASSERT_TRUE(codec.decode(adc_bytes, adc_round));
  TEST_ASSERT_EQUAL_UINT32(3000U, adc_round.reference_mv);
}

// Normalized profiles must be accepted by the shared profile validation and
// persistence path without changing the legacy gas/ADC acceptance.
void test_shared_codec_accepts_normalized_and_still_validates_legacy() {
  ProfileCodec codec{};
  const auto profile = clean_air_profile();
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(profile, encoded));
  TEST_ASSERT_TRUE(codec.validate(encoded, kMq135GasKey));
  TEST_ASSERT_TRUE(codec.validate(encoded, ProfileKey{ProfileKind::Calibration, ProfileTarget::Mq135}));

  EncodedProfile legacy{};
  TEST_ASSERT_TRUE(codec.encode(legacy_profile(), legacy));
  TEST_ASSERT_TRUE(codec.validate(legacy, kMq135GasKey));
}

void test_normalized_ab_commit_faults_restart_and_rollback() {
  ProfileCodec codec{};
  const auto first_profile = clean_air_profile();
  EncodedProfile first_bytes{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(first_profile, first_bytes));

  auto second_profile = first_profile;
  second_profile.manifest.profile_version = 4U;
  second_profile.q0 = 1.24F;
  EncodedProfile second_bytes{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(second_profile, second_bytes));

  MemoryProfileStore store{};
  ProfilePersistence writer{store, codec};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(writer.commit(kMq135GasKey, first_bytes)));
  const auto active_is_first = [&store, &codec, &first_bytes]() {
    ProfilePersistence restarted{store, codec};
    EncodedProfile active{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                          static_cast<int>(restarted.load(kMq135GasKey, active)));
    TEST_ASSERT_EQUAL_MEMORY(first_bytes.bytes.data(), active.bytes.data(),
                             first_bytes.size);
  };

  store.fail_next(MemoryProfileStore::Fault::WriteSlot);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::WriteFailed),
                        static_cast<int>(writer.commit(kMq135GasKey, second_bytes)));
  active_is_first();

  // Corrupting the inactive slot simulates a readback integrity failure; active
  // A must remain selected until a fully verified B and activation are durable.
  store.fail_next(MemoryProfileStore::Fault::CorruptAfterWrite);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::IntegrityFailed),
                        static_cast<int>(writer.commit(kMq135GasKey, second_bytes)));
  active_is_first();

  store.fail_next(MemoryProfileStore::Fault::WriteActivation);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::ActivationFailed),
                        static_cast<int>(writer.commit(kMq135GasKey, second_bytes)));
  active_is_first();

  // With no active slot, ReadSlot faults the actual post-write readback.
  MemoryProfileStore readback_store{};
  ProfilePersistence readback_writer{readback_store, codec};
  readback_store.fail_next(MemoryProfileStore::Fault::ReadSlot);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::ReadbackFailed),
                        static_cast<int>(readback_writer.commit(kMq135GasKey,
                                                               first_bytes)));
  ProfilePersistence after_failed_readback{readback_store, codec};
  EncodedProfile absent{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::NotFound),
                        static_cast<int>(after_failed_readback.load(kMq135GasKey,
                                                                   absent)));

  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(writer.commit(kMq135GasKey, second_bytes)));
  ProfilePersistence restarted{store, codec};
  EncodedProfile active{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(restarted.load(kMq135GasKey, active)));
  TEST_ASSERT_EQUAL_MEMORY(second_bytes.bytes.data(), active.bytes.data(),
                           second_bytes.size);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(restarted.rollback(kMq135GasKey)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileReadStatus::Found),
                        static_cast<int>(restarted.load(kMq135GasKey, active)));
  TEST_ASSERT_EQUAL_MEMORY(first_bytes.bytes.data(), active.bytes.data(),
                           first_bytes.size);
}

// Guard rails: only the intended invariant is broken in each case.
void test_normalized_profile_rejects_each_malformed_variant() {
  EncodedProfile good{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(clean_air_profile(), good));
  Mq135NormalizedProfile out{};

  // Unknown reference-kind enum.
  EncodedProfile bad = good;
  bad.bytes[22U + 233U] = 9U;
  bad.bytes[bad.size - 1U] ^= 0xFFU;  // invalidate digest so the guard is the enum
  TEST_ASSERT_FALSE(decode_mq135_normalized_profile(bad, out));

  // Non-finite q0 with a recomputed digest is still rejected by validity.
  Mq135NormalizedProfile nan_profile = clean_air_profile();
  nan_profile.q0 = NAN;
  EncodedProfile nan_bytes{};
  TEST_ASSERT_FALSE(encode_mq135_normalized_profile(nan_profile, nan_bytes));

  // Contradictory bounds.
  Mq135NormalizedProfile bounds = clean_air_profile();
  bounds.minimum_q = 2.0F;
  bounds.maximum_q = 1.0F;
  EncodedProfile bounds_bytes{};
  TEST_ASSERT_FALSE(encode_mq135_normalized_profile(bounds, bounds_bytes));

  // Clean-air claim without the >48 h operator-declared preheat.
  Mq135NormalizedProfile preheat = clean_air_profile();
  preheat.heating_duration_us = kMq135MinimumInitialPreheatUs;  // exactly 48 h
  EncodedProfile preheat_bytes{};
  TEST_ASSERT_FALSE(encode_mq135_normalized_profile(preheat, preheat_bytes));

  // Mismatched curve binding.
  Mq135NormalizedProfile curve = clean_air_profile();
  curve.curve_source_sha256[0] ^= 0x01U;
  EncodedProfile curve_bytes{};
  TEST_ASSERT_FALSE(encode_mq135_normalized_profile(curve, curve_bytes));

  // Wrong payload version inside a valid-looking envelope.
  EncodedProfile version = good;
  version.bytes[22U] = 1U;
  TEST_ASSERT_FALSE(mq135_normalized_payload(version));
  TEST_ASSERT_FALSE(decode_mq135_normalized_profile(version, out));

  // Corrupted digest.
  EncodedProfile digest = good;
  digest.bytes[digest.size - 1U] ^= 0x01U;
  TEST_ASSERT_FALSE(decode_mq135_normalized_profile(digest, out));

  // Absent temperature flag with non-zero backing bytes is contradictory.
  Mq135NormalizedProfile absent = clean_air_profile();
  absent.temperature_present = false;
  absent.temperature_centi_c = 2537;
  EncodedProfile absent_bytes{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(absent, absent_bytes));
  TEST_ASSERT_FALSE(decode_mq135_normalized_profile(absent_bytes, out));

  // Optional T/RH may be independently absent with canonical zero backing.
  Mq135NormalizedProfile optional = clean_air_profile();
  optional.humidity_present = false;
  optional.humidity_centi_pct = 0U;
  EncodedProfile optional_bytes{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(optional, optional_bytes));
  TEST_ASSERT_TRUE(decode_mq135_normalized_profile(optional_bytes, out));
  TEST_ASSERT_FALSE(out.humidity_present);
  TEST_ASSERT_EQUAL_UINT32(0U, out.humidity_centi_pct);
  TEST_ASSERT_TRUE(out.temperature_present);
}

// OPERATIONAL keeps q0/ratio provenance without asserting clean air or preheat.
void test_operational_baseline_needs_no_clean_air_or_preheat_claim() {
  Mq135NormalizedProfile operational = clean_air_profile();
  operational.reference_kind = Mq135ReferenceKind::Operational;
  operational.preparation_confirmed = false;
  operational.preparation_method = Mq135PreparationMethod::Unknown;
  operational.heating_duration_us = 0U;
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(encode_mq135_normalized_profile(operational, encoded));
  Mq135NormalizedProfile decoded{};
  TEST_ASSERT_TRUE(decode_mq135_normalized_profile(encoded, decoded));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Mq135ReferenceKind::Operational),
                          static_cast<uint8_t>(decoded.reference_kind));
  TEST_ASSERT_FALSE(decoded.preparation_confirmed);
}

// The declared 0.6 divider is applied exactly once: AO = pin / attenuation.
void test_declared_divider_attenuation_is_applied_once() {
  const Mq135NormalizedElectrical electrical{5000U, 10000U, 15000U,
                                             Mq135Topology::DirectLoaded, true};
  TEST_ASSERT_TRUE(electrical.valid());
  TEST_ASSERT_EQUAL_FLOAT(0.6F, static_cast<float>(electrical.attenuation()));
  TEST_ASSERT_EQUAL_FLOAT(2500.0F,
                          static_cast<float>(1500.0 / electrical.attenuation()));
  const Mq135NormalizedElectrical missing_rl{5000U, 10000U, 15000U,
                                             Mq135Topology::Buffered, true};
  TEST_ASSERT_TRUE(missing_rl.valid());  // no RL member is required
  const Mq135NormalizedElectrical unconfirmed{5000U, 10000U, 15000U,
                                              Mq135Topology::Buffered, false};
  TEST_ASSERT_FALSE(unconfirmed.valid());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_normalized_profile_envelope_is_exactly_373_payload_427_envelope);
  RUN_TEST(test_normalized_profile_round_trip_and_reencode_are_stable);
  RUN_TEST(test_legacy_payload_v1_and_adc_bytes_are_retained);
  RUN_TEST(test_shared_codec_accepts_normalized_and_still_validates_legacy);
  RUN_TEST(test_normalized_ab_commit_faults_restart_and_rollback);
  RUN_TEST(test_normalized_profile_rejects_each_malformed_variant);
  RUN_TEST(test_operational_baseline_needs_no_clean_air_or_preheat_claim);
  RUN_TEST(test_declared_divider_attenuation_is_applied_once);
  return UNITY_END();
}
