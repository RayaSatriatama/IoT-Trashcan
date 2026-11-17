// S2 normalized MQ-135 profile codec.
//
// Deliberately a separate, coherent helper instead of growing the legacy
// profile_store.cpp. The SGP1 envelope framing, digest and constants are reused
// verbatim so the persisted container is identical; only the payload body
// differs (discriminator byte 2 at payload offset 0).

#include "acquisition/mq135_normalized.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "acquisition/profile_store.h"
#include "telemetry/sha256.h"

namespace sigap::acquisition {
namespace {

using telemetry_contract::sha256;

// Exact released payload offsets (S2_PROFILE_LAYOUT_RELEASE.json).
constexpr std::size_t kOffPayloadVersion{0U};
constexpr std::size_t kOffCircuitMv{1U};
constexpr std::size_t kOffDividerTopOhm{5U};
constexpr std::size_t kOffDividerBottomOhm{9U};
constexpr std::size_t kOffTopology{13U};
constexpr std::size_t kOffHardwareConfirmed{14U};
constexpr std::size_t kOffAdcHash{15U};
constexpr std::size_t kOffAdcVersion{47U};
constexpr std::size_t kOffQ0{51U};
constexpr std::size_t kOffMinimumQ{55U};
constexpr std::size_t kOffMaximumQ{59U};
constexpr std::size_t kOffReferencePinMv{63U};
constexpr std::size_t kOffReferenceAoMv{67U};
constexpr std::size_t kOffVariationPercent{71U};
constexpr std::size_t kOffSampleCount{75U};
constexpr std::size_t kOffRejectedCount{77U};
constexpr std::size_t kOffFirstUs{79U};
constexpr std::size_t kOffLastUs{87U};
constexpr std::size_t kOffEvidenceHash{95U};
constexpr std::size_t kOffRequiredSamples{127U};
constexpr std::size_t kOffMinDurationUs{129U};
constexpr std::size_t kOffMaxAgeUs{137U};
constexpr std::size_t kOffMaxVariationPercent{145U};
constexpr std::size_t kOffHardwareIdentity{149U};
constexpr std::size_t kOffReferenceEnvironment{181U};
constexpr std::size_t kOffTemperaturePresent{213U};
constexpr std::size_t kOffTemperatureCentiC{214U};
constexpr std::size_t kOffHumidityPresent{218U};
constexpr std::size_t kOffHumidityCentiPct{219U};
constexpr std::size_t kOffHeatingDurationUs{223U};
constexpr std::size_t kOffPreparationConfirmed{231U};
constexpr std::size_t kOffPreparationMethod{232U};
constexpr std::size_t kOffReferenceKind{233U};
constexpr std::size_t kOffAdcMethod{234U};
constexpr std::size_t kOffResolutionBits{235U};
constexpr std::size_t kOffAttenuationDb{236U};
constexpr std::size_t kOffAdcUnit{237U};
constexpr std::size_t kOffAdcChannel{238U};
constexpr std::size_t kOffCurveId{239U};
constexpr std::size_t kOffCurveVersion{241U};
constexpr std::size_t kOffCurveSourceSha256{243U};
constexpr std::size_t kOffEstimationMethod{275U};
constexpr std::size_t kOffProfileProvenance{276U};

constexpr std::size_t kPayloadSize{kMq135NormalizedPayloadBytes};

void put_u16(uint8_t* bytes, std::size_t offset, uint16_t value) noexcept {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1U] = static_cast<uint8_t>(value >> 8U);
}

void put_u32(uint8_t* bytes, std::size_t offset, uint32_t value) noexcept {
  for (unsigned i = 0; i < 4U; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8U * i));
}

void put_u64(uint8_t* bytes, std::size_t offset, uint64_t value) noexcept {
  for (unsigned i = 0; i < 8U; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8U * i));
}

void put_f32(uint8_t* bytes, std::size_t offset, float value) noexcept {
  uint32_t bits{0U};
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(bytes, offset, bits);
}

uint16_t get_u16(const uint8_t* bytes, std::size_t offset) noexcept {
  return static_cast<uint16_t>(bytes[offset]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1U]) << 8U);
}

uint32_t get_u32(const uint8_t* bytes, std::size_t offset) noexcept {
  uint32_t value{0U};
  for (unsigned i = 0; i < 4U; ++i) value |= static_cast<uint32_t>(bytes[offset + i]) << (8U * i);
  return value;
}

uint64_t get_u64(const uint8_t* bytes, std::size_t offset) noexcept {
  uint64_t value{0U};
  for (unsigned i = 0; i < 8U; ++i) value |= static_cast<uint64_t>(bytes[offset + i]) << (8U * i);
  return value;
}

float get_f32(const uint8_t* bytes, std::size_t offset) noexcept {
  const uint32_t bits = get_u32(bytes, offset);
  float value{0.0F};
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Existing 97-byte canonical provenance encoding, re-implemented against the
// released offsets so this helper stays independent of the legacy codec body.
void put_provenance(uint8_t* bytes, std::size_t offset,
                    const ProfileProvenance& value) noexcept {
  bytes[offset] = value.present ? 1U : 0U;
  bytes[offset + 1U] = value.boot_id.present ? 1U : 0U;
  std::copy(value.boot_id.bytes.begin(), value.boot_id.bytes.end(),
            bytes + offset + 2U);
  put_u64(bytes, offset + 18U, value.monotonic_us);
  bytes[offset + 26U] = value.recorded_at.available ? 1U : 0U;
  put_u64(bytes, offset + 27U, static_cast<uint64_t>(value.recorded_at.epoch_ms));
  bytes[offset + 35U] = static_cast<uint8_t>(value.time_quality);
  put_u64(bytes, offset + 36U, static_cast<uint64_t>(value.reference_primary));
  put_u64(bytes, offset + 44U, static_cast<uint64_t>(value.reference_secondary));
  put_u32(bytes, offset + 52U, value.evidence_count);
  put_u64(bytes, offset + 56U, value.evidence_sequence);
  bytes[offset + 64U] = value.units;
  std::copy(value.reference_note.begin(), value.reference_note.end(),
            bytes + offset + 65U);
}

bool get_provenance(const uint8_t* bytes, std::size_t offset,
                    ProfileProvenance& value) noexcept {
  const uint8_t present = bytes[offset];
  const uint8_t boot_present = bytes[offset + 1U];
  const uint8_t recorded = bytes[offset + 26U];
  const uint8_t time_quality = bytes[offset + 35U];
  if (present > 1U || boot_present > 1U || recorded > 1U ||
      time_quality > static_cast<uint8_t>(TimeQuality::Estimated)) {
    return false;
  }
  value.present = present != 0U;
  value.boot_id.present = boot_present != 0U;
  std::copy(bytes + offset + 2U, bytes + offset + 18U, value.boot_id.bytes.begin());
  value.monotonic_us = get_u64(bytes, offset + 18U);
  value.recorded_at.available = recorded != 0U;
  value.recorded_at.epoch_ms = static_cast<int64_t>(get_u64(bytes, offset + 27U));
  value.time_quality = static_cast<TimeQuality>(time_quality);
  value.reference_primary = static_cast<int64_t>(get_u64(bytes, offset + 36U));
  value.reference_secondary = static_cast<int64_t>(get_u64(bytes, offset + 44U));
  value.evidence_count = get_u32(bytes, offset + 52U);
  value.evidence_sequence = get_u64(bytes, offset + 56U);
  value.units = bytes[offset + 64U];
  std::copy(bytes + offset + 65U, bytes + offset + 97U,
            value.reference_note.begin());
  return true;
}

bool parse_envelope(const EncodedProfile& encoded, std::size_t& payload_offset,
                    std::size_t& payload_length) noexcept {
  if (encoded.size < kProfileHeaderBytes + kProfileHashBytes ||
      encoded.size > encoded.bytes.size()) {
    return false;
  }
  const uint8_t* bytes = encoded.bytes.data();
  if (get_u32(bytes, 0U) != kProfileMagic ||
      get_u16(bytes, 4U) != kProfileFormatVersion ||
      get_u32(bytes, 6U) != kProfileSchemaVersion ||
      get_u32(bytes, 10U) != static_cast<uint32_t>(ProfileTarget::Mq135) ||
      get_u32(bytes, 14U) == 0U ||
      bytes[18U] != static_cast<uint8_t>(ProfileKind::Calibration) ||
      bytes[19U] > static_cast<uint8_t>(ProfileValidity::TestOnly)) {
    return false;
  }
  payload_length = get_u16(bytes, 20U);
  if (payload_length != kPayloadSize ||
      encoded.size != kProfileHeaderBytes + payload_length + kProfileHashBytes) {
    return false;
  }
  const auto digest = sha256(std::string_view(
      reinterpret_cast<const char*>(bytes), kProfileHeaderBytes + payload_length));
  if (!std::equal(digest.begin(), digest.end(),
                  bytes + kProfileHeaderBytes + payload_length)) {
    return false;
  }
  payload_offset = kProfileHeaderBytes;
  return true;
}

}  // namespace

bool mq135_normalized_payload(const EncodedProfile& encoded) noexcept {
  std::size_t offset{0U};
  std::size_t length{0U};
  if (!parse_envelope(encoded, offset, length)) return false;
  return encoded.bytes[offset + kOffPayloadVersion] == kMq135NormalizedPayloadVersion;
}

bool encode_mq135_normalized_profile(const Mq135NormalizedProfile& profile,
                                     EncodedProfile& output) noexcept {
  if (!profile.valid()) return false;
  output = EncodedProfile{};
  uint8_t* bytes = output.bytes.data();
  put_u32(bytes, 0U, kProfileMagic);
  put_u16(bytes, 4U, kProfileFormatVersion);
  put_u32(bytes, 6U, kProfileSchemaVersion);
  put_u32(bytes, 10U, static_cast<uint32_t>(ProfileTarget::Mq135));
  put_u32(bytes, 14U, profile.manifest.profile_version);
  bytes[18U] = static_cast<uint8_t>(ProfileKind::Calibration);
  bytes[19U] = static_cast<uint8_t>(profile.manifest.validity);
  put_u16(bytes, 20U, static_cast<uint16_t>(kPayloadSize));

  uint8_t* p = bytes + kProfileHeaderBytes;
  p[kOffPayloadVersion] = kMq135NormalizedPayloadVersion;
  put_u32(p, kOffCircuitMv, profile.electrical.circuit_mv);
  put_u32(p, kOffDividerTopOhm, profile.electrical.divider_top_ohm);
  put_u32(p, kOffDividerBottomOhm, profile.electrical.divider_bottom_ohm);
  p[kOffTopology] = static_cast<uint8_t>(profile.electrical.topology);
  p[kOffHardwareConfirmed] = profile.electrical.hardware_confirmed ? 1U : 0U;
  std::copy(profile.adc_hash.begin(), profile.adc_hash.end(), p + kOffAdcHash);
  put_u32(p, kOffAdcVersion, profile.adc_version);
  put_f32(p, kOffQ0, profile.q0);
  put_f32(p, kOffMinimumQ, profile.minimum_q);
  put_f32(p, kOffMaximumQ, profile.maximum_q);
  put_u32(p, kOffReferencePinMv, profile.reference_pin_mv);
  put_u32(p, kOffReferenceAoMv, profile.reference_ao_mv);
  put_f32(p, kOffVariationPercent, profile.variation_percent);
  put_u16(p, kOffSampleCount, profile.sample_count);
  put_u16(p, kOffRejectedCount, profile.rejected_count);
  put_u64(p, kOffFirstUs, profile.first_us);
  put_u64(p, kOffLastUs, profile.last_us);
  std::copy(profile.evidence_hash.begin(), profile.evidence_hash.end(),
            p + kOffEvidenceHash);
  put_u16(p, kOffRequiredSamples, profile.criteria.samples);
  put_u64(p, kOffMinDurationUs, profile.criteria.min_duration_us);
  put_u64(p, kOffMaxAgeUs, profile.criteria.max_age_us);
  put_f32(p, kOffMaxVariationPercent, profile.criteria.max_variation_percent);
  std::copy(profile.hardware_identity.begin(), profile.hardware_identity.end(),
            p + kOffHardwareIdentity);
  std::copy(profile.reference_environment.begin(),
            profile.reference_environment.end(), p + kOffReferenceEnvironment);
  p[kOffTemperaturePresent] = profile.temperature_present ? 1U : 0U;
  put_u32(p, kOffTemperatureCentiC,
          static_cast<uint32_t>(profile.temperature_centi_c));
  p[kOffHumidityPresent] = profile.humidity_present ? 1U : 0U;
  put_u32(p, kOffHumidityCentiPct, profile.humidity_centi_pct);
  put_u64(p, kOffHeatingDurationUs, profile.heating_duration_us);
  p[kOffPreparationConfirmed] = profile.preparation_confirmed ? 1U : 0U;
  p[kOffPreparationMethod] = static_cast<uint8_t>(profile.preparation_method);
  p[kOffReferenceKind] = static_cast<uint8_t>(profile.reference_kind);
  p[kOffAdcMethod] = profile.adc_method;
  p[kOffResolutionBits] = profile.resolution_bits;
  p[kOffAttenuationDb] = profile.attenuation_db;
  p[kOffAdcUnit] = profile.adc_unit;
  p[kOffAdcChannel] = profile.adc_channel;
  put_u16(p, kOffCurveId, profile.curve_id);
  put_u16(p, kOffCurveVersion, profile.curve_version);
  std::copy(profile.curve_source_sha256.begin(),
            profile.curve_source_sha256.end(), p + kOffCurveSourceSha256);
  p[kOffEstimationMethod] = static_cast<uint8_t>(profile.estimation_method);
  put_provenance(p, kOffProfileProvenance, profile.manifest.provenance);

  const std::size_t payload_end = kProfileHeaderBytes + kPayloadSize;
  const auto digest = sha256(std::string_view(
      reinterpret_cast<const char*>(bytes), payload_end));
  std::copy(digest.begin(), digest.end(), bytes + payload_end);
  output.size = payload_end + kProfileHashBytes;
  return true;
}

bool decode_mq135_normalized_profile(const EncodedProfile& encoded,
                                     Mq135NormalizedProfile& output) noexcept {
  std::size_t payload_offset{0U};
  std::size_t payload_length{0U};
  if (!parse_envelope(encoded, payload_offset, payload_length)) return false;
  const uint8_t* p = encoded.bytes.data() + payload_offset;
  if (p[kOffPayloadVersion] != kMq135NormalizedPayloadVersion) return false;

  Mq135NormalizedProfile profile{};
  profile.manifest.kind = ProfileKind::Calibration;
  profile.manifest.schema_version = kProfileSchemaVersion;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Mq135);
  profile.manifest.profile_version = get_u32(encoded.bytes.data(), 14U);
  profile.manifest.validity = static_cast<ProfileValidity>(encoded.bytes[19U]);
  profile.manifest.hash = encoded.hash();

  const uint8_t topology = p[kOffTopology];
  const uint8_t hardware_confirmed = p[kOffHardwareConfirmed];
  const uint8_t temperature_present = p[kOffTemperaturePresent];
  const uint8_t humidity_present = p[kOffHumidityPresent];
  const uint8_t preparation_confirmed = p[kOffPreparationConfirmed];
  const uint8_t preparation_method = p[kOffPreparationMethod];
  const uint8_t reference_kind = p[kOffReferenceKind];
  const uint8_t estimation_method = p[kOffEstimationMethod];
  if (hardware_confirmed > 1U || temperature_present > 1U ||
      humidity_present > 1U || preparation_confirmed > 1U ||
      topology > static_cast<uint8_t>(Mq135Topology::Buffered) ||
      preparation_method > static_cast<uint8_t>(Mq135PreparationMethod::OperatorDeclared) ||
      reference_kind > static_cast<uint8_t>(Mq135ReferenceKind::CleanAirConfirmed) ||
      estimation_method > static_cast<uint8_t>(Mq135EstimationMethod::PiecewiseLogLog)) {
    return false;
  }

  profile.electrical.circuit_mv = get_u32(p, kOffCircuitMv);
  profile.electrical.divider_top_ohm = get_u32(p, kOffDividerTopOhm);
  profile.electrical.divider_bottom_ohm = get_u32(p, kOffDividerBottomOhm);
  profile.electrical.topology = static_cast<Mq135Topology>(topology);
  profile.electrical.hardware_confirmed = hardware_confirmed != 0U;
  std::copy(p + kOffAdcHash, p + kOffAdcHash + 32U, profile.adc_hash.begin());
  profile.adc_version = get_u32(p, kOffAdcVersion);
  profile.q0 = get_f32(p, kOffQ0);
  profile.minimum_q = get_f32(p, kOffMinimumQ);
  profile.maximum_q = get_f32(p, kOffMaximumQ);
  profile.reference_pin_mv = get_u32(p, kOffReferencePinMv);
  profile.reference_ao_mv = get_u32(p, kOffReferenceAoMv);
  profile.variation_percent = get_f32(p, kOffVariationPercent);
  profile.sample_count = get_u16(p, kOffSampleCount);
  profile.rejected_count = get_u16(p, kOffRejectedCount);
  profile.first_us = get_u64(p, kOffFirstUs);
  profile.last_us = get_u64(p, kOffLastUs);
  std::copy(p + kOffEvidenceHash, p + kOffEvidenceHash + 32U,
            profile.evidence_hash.begin());
  profile.criteria.samples = get_u16(p, kOffRequiredSamples);
  profile.criteria.min_duration_us = get_u64(p, kOffMinDurationUs);
  profile.criteria.max_age_us = get_u64(p, kOffMaxAgeUs);
  profile.criteria.max_variation_percent = get_f32(p, kOffMaxVariationPercent);
  std::copy(p + kOffHardwareIdentity, p + kOffHardwareIdentity + 32U,
            profile.hardware_identity.begin());
  std::copy(p + kOffReferenceEnvironment, p + kOffReferenceEnvironment + 32U,
            profile.reference_environment.begin());
  profile.temperature_present = temperature_present != 0U;
  profile.temperature_centi_c = static_cast<int32_t>(get_u32(p, kOffTemperatureCentiC));
  profile.humidity_present = humidity_present != 0U;
  profile.humidity_centi_pct = get_u32(p, kOffHumidityCentiPct);
  profile.heating_duration_us = get_u64(p, kOffHeatingDurationUs);
  profile.preparation_confirmed = preparation_confirmed != 0U;
  profile.preparation_method = static_cast<Mq135PreparationMethod>(preparation_method);
  profile.reference_kind = static_cast<Mq135ReferenceKind>(reference_kind);
  profile.adc_method = p[kOffAdcMethod];
  profile.resolution_bits = p[kOffResolutionBits];
  profile.attenuation_db = p[kOffAttenuationDb];
  profile.adc_unit = p[kOffAdcUnit];
  profile.adc_channel = p[kOffAdcChannel];
  profile.curve_id = get_u16(p, kOffCurveId);
  profile.curve_version = get_u16(p, kOffCurveVersion);
  std::copy(p + kOffCurveSourceSha256, p + kOffCurveSourceSha256 + 32U,
            profile.curve_source_sha256.begin());
  profile.estimation_method = static_cast<Mq135EstimationMethod>(estimation_method);
  if (!get_provenance(p, kOffProfileProvenance, profile.manifest.provenance)) {
    return false;
  }
  // A presented temperature/humidity flag must not carry the "absent" zero
  // backing convention silently; and an absent flag must keep canonical zeros.
  if (!profile.temperature_present && profile.temperature_centi_c != 0) return false;
  if (!profile.humidity_present && profile.humidity_centi_pct != 0U) return false;
  if (!profile.valid()) return false;
  output = profile;
  return true;
}

}  // namespace sigap::acquisition
