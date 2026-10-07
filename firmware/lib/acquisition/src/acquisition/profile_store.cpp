#include "acquisition/profile_store.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

#include "../../../telemetry_contract/src/telemetry/sha256.h"
#include "acquisition/target_protocol.h"

#ifdef ARDUINO
#include <nvs.h>
#endif

namespace sigap::acquisition {
namespace {

using telemetry_contract::sha256;

void put_u32_le(uint8_t* bytes, std::size_t offset, uint32_t value) noexcept {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1U] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2U] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

uint32_t get_u32_le(const uint8_t* bytes, std::size_t offset) noexcept {
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

config::ConfigHash metadata_integrity(const ProfileActivationMetadata& metadata) noexcept {
  std::array<uint8_t, kActivationMetadataCoreBytes> bytes{};
  bytes[0] = kActivationMetadataFormatVersion;
  put_u32_le(bytes.data(), kActivationMetadataGenerationOffset, metadata.generation);
  bytes[kActivationMetadataHasActiveOffset] = metadata.has_active ? 1U : 0U;
  bytes[kActivationMetadataActiveSlotOffset] = static_cast<uint8_t>(metadata.active_slot);
  put_u32_le(bytes.data(), kActivationMetadataActiveVersionOffset, metadata.active_version);
  std::copy(metadata.active_hash.begin(), metadata.active_hash.end(),
            bytes.begin() + kActivationMetadataActiveHashOffset);
  bytes[kActivationMetadataHasPreviousOffset] = metadata.has_previous ? 1U : 0U;
  bytes[kActivationMetadataPreviousSlotOffset] = static_cast<uint8_t>(metadata.previous_slot);
  put_u32_le(bytes.data(), kActivationMetadataPreviousVersionOffset,
             metadata.previous_version);
  std::copy(metadata.previous_hash.begin(), metadata.previous_hash.end(),
            bytes.begin() + kActivationMetadataPreviousHashOffset);
  return sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
bool metadata_integrity_valid(const ProfileActivationMetadata& metadata) noexcept {
  return metadata.integrity == metadata_integrity(metadata);
}
class Writer final {
 public:
  explicit Writer(std::array<uint8_t, kEncodedProfileBytes>& bytes) noexcept
      : bytes_(bytes) {}

  bool u8(uint8_t value) noexcept { return put(&value, sizeof(value)); }
  bool u16(uint16_t value) noexcept {
    const std::array<uint8_t, 2> bytes{
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U)};
    return put(bytes.data(), bytes.size());
  }
  bool u32(uint32_t value) noexcept {
    std::array<uint8_t, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<uint8_t>(value >> (8U * i));
    }
    return put(bytes.data(), bytes.size());
  }
  bool u64(uint64_t value) noexcept {
    std::array<uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<uint8_t>(value >> (8U * i));
    }
    return put(bytes.data(), bytes.size());
  }
  bool i32(int32_t value) noexcept {
    return u32(static_cast<uint32_t>(value));
  }
  bool i64(int64_t value) noexcept {
    return u64(static_cast<uint64_t>(value));
  }
  bool f32(float value) noexcept {
    static_assert(sizeof(float) == sizeof(uint32_t), "profile float ABI");
    uint32_t bits{0};
    std::memcpy(&bits, &value, sizeof(bits));
    return u32(bits);
  }
  bool bytes(const uint8_t* source, std::size_t count) noexcept {
    return put(source, count);
  }
  bool provenance(const ProfileProvenance& value) noexcept {
    const auto boot_present = value.boot_id.present ? 1U : 0U;
    return u8(value.present ? 1U : 0U) && u8(boot_present) &&
           bytes(value.boot_id.bytes.data(), value.boot_id.bytes.size()) &&
           u64(value.monotonic_us) && u8(value.recorded_at.available ? 1U : 0U) &&
           i64(value.recorded_at.epoch_ms) && u8(static_cast<uint8_t>(value.time_quality)) &&
           i64(value.reference_primary) && i64(value.reference_secondary) &&
           u32(value.evidence_count) && u64(value.evidence_sequence) &&
           u8(value.units) &&
           bytes(reinterpret_cast<const uint8_t*>(value.reference_note.data()),
                 value.reference_note.size());
  }
  std::size_t offset() const noexcept { return offset_; }

 private:
  bool put(const uint8_t* source, std::size_t count) noexcept {
    if (source == nullptr || offset_ + count > bytes_.size()) return false;
    std::copy_n(source, count, bytes_.begin() + offset_);
    offset_ += count;
    return true;
  }

  std::array<uint8_t, kEncodedProfileBytes>& bytes_;
  std::size_t offset_{0U};
};
class Reader final {
 public:
  Reader(const uint8_t* bytes, std::size_t size) noexcept
      : bytes_(bytes), size_(size) {}

  bool u8(uint8_t& value) noexcept { return get(&value, sizeof(value)); }
  bool u16(uint16_t& value) noexcept {
    std::array<uint8_t, 2> bytes{};
    if (!get(bytes.data(), bytes.size())) return false;
    value = static_cast<uint16_t>(bytes[0]) |
            (static_cast<uint16_t>(bytes[1]) << 8U);
    return true;
  }
  bool u32(uint32_t& value) noexcept {
    std::array<uint8_t, 4> bytes{};
    if (!get(bytes.data(), bytes.size())) return false;
    value = 0U;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      value |= static_cast<uint32_t>(bytes[i]) << (8U * i);
    }
    return true;
  }
  bool u64(uint64_t& value) noexcept {
    std::array<uint8_t, 8> bytes{};
    if (!get(bytes.data(), bytes.size())) return false;
    value = 0U;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      value |= static_cast<uint64_t>(bytes[i]) << (8U * i);
    }
    return true;
  }
  bool i32(int32_t& value) noexcept {
    uint32_t raw{0U};
    if (!u32(raw)) return false;
    std::memcpy(&value, &raw, sizeof(value));
    return true;
  }
  bool i64(int64_t& value) noexcept {
    uint64_t raw{0U};
    if (!u64(raw)) return false;
    std::memcpy(&value, &raw, sizeof(value));
    return true;
  }
  bool f32(float& value) noexcept {
    uint32_t raw{0U};
    if (!u32(raw)) return false;
    std::memcpy(&value, &raw, sizeof(value));
    return true;
  }
  bool bytes(uint8_t* destination, std::size_t count) noexcept {
    return get(destination, count);
  }
  bool provenance(ProfileProvenance& value) noexcept {
    uint8_t present{0U};
    uint8_t boot_present{0U};
    uint8_t recorded{0U};
    uint8_t time_quality{0U};
    if (!u8(present) || !u8(boot_present) ||
        !bytes(value.boot_id.bytes.data(), value.boot_id.bytes.size()) ||
        !u64(value.monotonic_us) || !u8(recorded) ||
        !i64(value.recorded_at.epoch_ms) || !u8(time_quality) ||
        !i64(value.reference_primary) || !i64(value.reference_secondary) ||
        !u32(value.evidence_count) || !u64(value.evidence_sequence) ||
        !u8(value.units) ||
        !bytes(reinterpret_cast<uint8_t*>(value.reference_note.data()),
               value.reference_note.size())) {
      return false;
    }
    if (present > 1U || boot_present > 1U || recorded > 1U ||
        time_quality > static_cast<uint8_t>(TimeQuality::Estimated)) {
      return false;
    }
    value.present = present != 0U;
    value.boot_id.present = boot_present != 0U;
    value.recorded_at.available = recorded != 0U;
    value.time_quality = static_cast<TimeQuality>(time_quality);
    return true;
  }
  std::size_t offset() const noexcept { return offset_; }
  std::size_t remaining() const noexcept {
    return offset_ <= size_ ? size_ - offset_ : 0U;
  }

 private:
  bool get(uint8_t* destination, std::size_t count) noexcept {
    if (destination == nullptr || bytes_ == nullptr || offset_ + count > size_) {
      return false;
    }
    std::copy_n(bytes_ + offset_, count, destination);
    offset_ += count;
    return true;
  }

  const uint8_t* bytes_{nullptr};
  std::size_t size_{0U};
  std::size_t offset_{0U};
};
struct ParsedHeader final {
  uint32_t schema{0U};
  ProfileKind kind{ProfileKind::Calibration};
  ProfileTarget target{ProfileTarget::Unknown};
  uint32_t version{0U};
  ProfileValidity validity{ProfileValidity::Missing};
  uint16_t payload_length{0U};
  std::size_t payload_offset{kProfileHeaderBytes};
};

bool read_optional_provenance(Reader& reader, ProfileProvenance& provenance) noexcept {
  if (reader.remaining() == 0U) {
    provenance = ProfileProvenance{};
    return true;
  }
  if (reader.remaining() != kProfileProvenanceBytes) return false;
  return reader.provenance(provenance);
}

bool parse_header(const EncodedProfile& encoded, ParsedHeader& header) noexcept {
  if (encoded.size < kProfileHeaderBytes + kProfileHashBytes ||
      encoded.size > encoded.bytes.size()) {
    return false;
  }
  Reader reader(encoded.bytes.data(), encoded.size);
  uint32_t magic{0U};
  uint16_t format{0U};
  uint32_t target{0U};
  uint8_t kind{0U};
  uint8_t validity{0U};
  if (!reader.u32(magic) || !reader.u16(format) || !reader.u32(header.schema) ||
      !reader.u32(target) || !reader.u32(header.version) ||
      !reader.u8(kind) || !reader.u8(validity) ||
      !reader.u16(header.payload_length)) {
    return false;
  }
  if (magic != kProfileMagic || format != kProfileFormatVersion ||
      header.schema != kProfileSchemaVersion ||
      header.payload_length > kProfilePayloadBytes ||
      encoded.size != kProfileHeaderBytes + header.payload_length + kProfileHashBytes) {
    return false;
  }
  header.target = static_cast<ProfileTarget>(target);
  header.kind = static_cast<ProfileKind>(kind);
  header.validity = static_cast<ProfileValidity>(validity);
  return header.kind >= ProfileKind::Calibration &&
         header.kind <= ProfileKind::Capacity &&
         header.validity <= ProfileValidity::TestOnly;
}
bool hash_matches(const EncodedProfile& encoded) noexcept {
  ParsedHeader header{};
  if (!parse_header(encoded, header)) return false;
  const auto digest = sha256(std::string_view(
      reinterpret_cast<const char*>(encoded.bytes.data()),
      kProfileHeaderBytes + header.payload_length));
  return std::equal(digest.begin(), digest.end(),
                    encoded.bytes.begin() + kProfileHeaderBytes +
                        header.payload_length);
}
bool begin_payload(const ProfileManifest& manifest, ProfileTarget fallback,
                   ProfileKind kind,
                   EncodedProfile& output, Writer& writer) noexcept {
  output = EncodedProfile{};
  const ProfileTarget target = manifest.target_id == 0U
                                   ? fallback
                                   : static_cast<ProfileTarget>(manifest.target_id);
  if (target == ProfileTarget::Unknown || manifest.schema_version == 0U ||
      manifest.profile_version == 0U) {
    return false;
  }
  if (!writer.u32(kProfileMagic) || !writer.u16(kProfileFormatVersion) ||
      !writer.u32(manifest.schema_version) ||
      !writer.u32(static_cast<uint32_t>(target)) ||
      !writer.u32(manifest.profile_version) ||
      !writer.u8(static_cast<uint8_t>(kind)) ||
      !writer.u8(static_cast<uint8_t>(manifest.validity)) ||
      !writer.u16(0U)) {
    return false;
  }
  return true;
}
void finish_encoded(EncodedProfile& output, std::size_t payload_end) noexcept {
  const std::size_t payload_length = payload_end - kProfileHeaderBytes;
  output.bytes[20U] = static_cast<uint8_t>(payload_length);
  output.bytes[21U] = static_cast<uint8_t>(payload_length >> 8U);
  const auto digest = sha256(std::string_view(
      reinterpret_cast<const char*>(output.bytes.data()), payload_end));
  std::copy(digest.begin(), digest.end(), output.bytes.begin() + payload_end);
  output.size = payload_end + kProfileHashBytes;
}
ProfileManifest manifest_from(const ParsedHeader& header,
                              const config::ConfigHash& hash) noexcept {
  ProfileManifest manifest{};
  manifest.kind = header.kind;
  manifest.schema_version = header.schema;
  manifest.profile_version = header.version;
  manifest.target_id = static_cast<uint32_t>(header.target);
  manifest.validity = header.validity;
  manifest.hash = hash;
  return manifest;
}

bool valid_target(ProfileTarget actual, ProfileTarget expected) noexcept {
  return expected == ProfileTarget::Unknown || actual == expected;
}

bool valid_key(const ParsedHeader& actual, ProfileKey expected) noexcept {
  return (expected.target == ProfileTarget::Unknown ||
          actual.target == expected.target) && actual.kind == expected.kind;
}

template <typename Profile>
bool decode_header(const EncodedProfile& encoded, ProfileTarget expected,
                   ParsedHeader& header, Profile& output) noexcept {
  if (!parse_header(encoded, header) || !hash_matches(encoded) ||
      !valid_target(header.target, expected)) {
    return false;
  }
  output = Profile{};
  output.manifest = manifest_from(header, encoded.hash());
  return true;
}

template <typename Profile>
bool decode_key_header(const EncodedProfile& encoded, ProfileKey expected,
                       ParsedHeader& header, Profile& output) noexcept {
  if (!parse_header(encoded, header) || !hash_matches(encoded) ||
      !valid_key(header, expected)) {
    return false;
  }
  output = Profile{};
  output.manifest = manifest_from(header, encoded.hash());
  return true;
}

}  // namespace

config::ConfigHash EncodedProfile::hash() const noexcept {
  config::ConfigHash result{};
  ParsedHeader header{};
  if (!parse_header(*this, header)) return result;
  std::copy_n(bytes.begin() + kProfileHeaderBytes + header.payload_length,
              kProfileHashBytes, result.begin());
  return result;
}

bool ProfileCodec::encode(const Hx711Profile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown,
                     ProfileKind::Calibration, output, writer) ||
      !writer.f32(profile.counts_per_kg) || !writer.i64(profile.tare_offset_counts) ||
      !writer.provenance(profile.manifest.provenance)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const UltrasonicProfile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown,
                     ProfileKind::Calibration, output, writer) ||
      !writer.f32(profile.d_empty_mm) || !writer.f32(profile.d_full_mm) ||
      !writer.f32(profile.echo_mm_per_us) ||
      !writer.f32(profile.overshoot_tolerance_mm) ||
      !writer.u32(profile.echo_timeout_us) ||
      !writer.u32(profile.inter_sensor_gap_us) ||
      !writer.provenance(profile.manifest.provenance)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const MqAdcProfile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown,
                     ProfileKind::Adc, output, writer) ||
      !writer.u32(profile.reference_mv) || !writer.u32(profile.full_scale_counts) ||
      !writer.u32(profile.divider_numerator) ||
      !writer.u32(profile.divider_denominator) ||
      !writer.provenance(profile.manifest.provenance)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const Mq7HeaterProfile& profile,
                          EncodedProfile& output) const noexcept {
  ProfileManifest manifest{};
  manifest.schema_version = profile.schema_version;
  manifest.profile_version = profile.profile_version;
  manifest.target_id = profile.target_id;
  manifest.hash = profile.hash;
  manifest.validity = profile.present ? ProfileValidity::Valid : ProfileValidity::Missing;
  manifest.provenance = profile.provenance;
  Writer writer(output.bytes);
  if (!begin_payload(manifest, ProfileTarget::Mq7,
                     ProfileKind::Heater, output, writer) ||
      !writer.u64(profile.preheat_us) || !writer.u64(profile.high_clean_us) ||
      !writer.u64(profile.low_detect_us) || !writer.provenance(manifest.provenance)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const Ds18b20Profile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown,
                     ProfileKind::Calibration, output, writer) ||
      !writer.u8(profile.has_rom ? 1U : 0U) || !writer.bytes(profile.rom.data(), profile.rom.size()) ||
      !writer.u8(profile.resolution_bits) || !writer.i32(profile.offset_centi_c) ||
      !writer.provenance(profile.manifest.provenance)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const Sht31Profile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown,
                     ProfileKind::Calibration, output, writer) ||
      !writer.u8(profile.address) || !writer.i32(profile.temperature_offset_centi_c) ||
      !writer.i32(profile.humidity_offset_centi_pct) ||
      !writer.provenance(profile.manifest.provenance)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const OperationalCapacityProfile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown,
                     ProfileKind::Capacity, output, writer) ||
      !writer.u8(static_cast<uint8_t>(profile.stream)) ||
      !writer.f32(profile.operational_capacity_kg) ||
      !writer.provenance(profile.manifest.provenance)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          Hx711Profile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, static_cast<ProfileTarget>(output.manifest.target_id), h,
                     output)) return false;
  if (h.kind != ProfileKind::Calibration ||
      h.target < ProfileTarget::Hx711Organic || h.target > ProfileTarget::Hx711B3) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.f32(output.counts_per_kg) && reader.i64(output.tare_offset_counts) &&
         read_optional_provenance(reader, output.manifest.provenance) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          UltrasonicProfile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Unknown, h, output) ||
      h.kind != ProfileKind::Calibration ||
      h.target < ProfileTarget::UltrasonicOrganic || h.target > ProfileTarget::UltrasonicB3) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.f32(output.d_empty_mm) && reader.f32(output.d_full_mm) &&
         reader.f32(output.echo_mm_per_us) && reader.f32(output.overshoot_tolerance_mm) &&
         reader.u32(output.echo_timeout_us) && reader.u32(output.inter_sensor_gap_us) &&
         read_optional_provenance(reader, output.manifest.provenance) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          MqAdcProfile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Unknown, h, output) ||
      h.kind != ProfileKind::Adc ||
      h.target < ProfileTarget::Mq4 || h.target > ProfileTarget::Mq8) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.u32(output.reference_mv) && reader.u32(output.full_scale_counts) &&
         reader.u32(output.divider_numerator) && reader.u32(output.divider_denominator) &&
         read_optional_provenance(reader, output.manifest.provenance) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          Mq7HeaterProfile& output) const noexcept {
  ParsedHeader h{};
  if (!parse_header(encoded, h) || !hash_matches(encoded) ||
      h.kind != ProfileKind::Heater ||
      h.target != ProfileTarget::Mq7) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  if (!reader.u64(output.preheat_us) || !reader.u64(output.high_clean_us) ||
      !reader.u64(output.low_detect_us) ||
      !read_optional_provenance(reader, output.provenance) ||
      reader.offset() != h.payload_length) {
    return false;
  }
  const auto hash = encoded.hash();
  output.schema_version = h.schema;
  output.profile_version = h.version;
  output.target_id = static_cast<uint32_t>(h.target);
  output.hash = hash;
  output.present = h.validity == ProfileValidity::Valid ||
                   h.validity == ProfileValidity::TestOnly;
  return true;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          Ds18b20Profile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Ds18b20B3, h, output) ||
      h.kind != ProfileKind::Calibration) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  uint8_t has_rom{0U};
  if (!reader.u8(has_rom) || !reader.bytes(output.rom.data(), output.rom.size()) ||
      !reader.u8(output.resolution_bits) || !reader.i32(output.offset_centi_c)) return false;
  output.has_rom = has_rom != 0U;
  return read_optional_provenance(reader, output.manifest.provenance) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          Sht31Profile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Unknown, h, output) ||
      h.kind != ProfileKind::Calibration ||
      (h.target != ProfileTarget::Sht31_44 && h.target != ProfileTarget::Sht31_45)) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.u8(output.address) && reader.i32(output.temperature_offset_centi_c) &&
         reader.i32(output.humidity_offset_centi_pct) &&
         read_optional_provenance(reader, output.manifest.provenance) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          OperationalCapacityProfile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Unknown, h, output) ||
      h.kind != ProfileKind::Capacity ||
      h.target < ProfileTarget::Hx711Organic ||
      h.target > ProfileTarget::Hx711B3) {
    return false;
  }
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  uint8_t stream{0U};
  if (!reader.u8(stream) || stream > static_cast<uint8_t>(StreamId::B3) ||
      !reader.f32(output.operational_capacity_kg) ||
      !read_optional_provenance(reader, output.manifest.provenance) ||
      reader.offset() != h.payload_length) {
    return false;
  }
  output.stream = static_cast<StreamId>(stream);
  output.present = h.validity == ProfileValidity::Valid ||
                   h.validity == ProfileValidity::TestOnly;
  return true;
}

bool ProfileCodec::validate(const EncodedProfile& encoded,
                            ProfileKey expected) const noexcept {
  ParsedHeader header{};
  if (!parse_header(encoded, header) || !hash_matches(encoded) ||
      !valid_key(header, expected) || header.version == 0U ||
      (header.validity != ProfileValidity::Valid &&
       header.validity != ProfileValidity::TestOnly)) {
    return false;
  }
  switch (expected.kind) {
    case ProfileKind::Calibration: {
      if (header.target >= ProfileTarget::Hx711Organic &&
          header.target <= ProfileTarget::Hx711B3) {
        Hx711Profile profile{};
        return decode(encoded, profile) && profile.valid_for_conversion();
      }
      if (header.target >= ProfileTarget::UltrasonicOrganic &&
          header.target <= ProfileTarget::UltrasonicB3) {
        UltrasonicProfile profile{};
        return decode(encoded, profile) && profile.valid_for_conversion();
      }
      if (header.target == ProfileTarget::Ds18b20B3) {
        Ds18b20Profile profile{};
        return decode(encoded, profile) && profile.valid_for_conversion() &&
               ds18b20_crc8(profile.rom.data(), 7U) == profile.rom[7];
      }
      if (header.target == ProfileTarget::Sht31_44 ||
          header.target == ProfileTarget::Sht31_45) {
        Sht31Profile profile{};
        return decode(encoded, profile) && profile.valid_for_conversion() &&
               ((header.target == ProfileTarget::Sht31_44 && profile.address == 0x44U) ||
                (header.target == ProfileTarget::Sht31_45 && profile.address == 0x45U));
      }
      return false;
    }
    case ProfileKind::Adc: {
      if (header.target < ProfileTarget::Mq4 || header.target > ProfileTarget::Mq8) {
        return false;
      }
      MqAdcProfile profile{};
      return decode(encoded, profile) && profile.valid_for_conversion();
    }
    case ProfileKind::Heater: {
      if (header.target != ProfileTarget::Mq7) return false;
      Mq7HeaterProfile profile{};
      return decode(encoded, profile) && profile.valid();
    }
    case ProfileKind::Capacity: {
      if (header.target < ProfileTarget::Hx711Organic ||
          header.target > ProfileTarget::Hx711B3) {
        return false;
      }
      OperationalCapacityProfile profile{};
      const auto expected_stream =
          header.target == ProfileTarget::Hx711Organic
              ? StreamId::Organic
              : header.target == ProfileTarget::Hx711Inorganic
                    ? StreamId::Inorganic
                    : StreamId::B3;
      return decode(encoded, profile) && profile.valid_for_normalization() &&
             profile.stream == expected_stream;
    }
    default:
      return false;
  }
}

bool ProfileCodec::validate(const EncodedProfile& encoded,
                            ProfileTarget expected) const noexcept {
  if (expected != ProfileTarget::Unknown) {
    return validate(encoded, default_profile_key(expected));
  }
  ParsedHeader header{};
  if (!parse_header(encoded, header) || !hash_matches(encoded) ||
      header.target == ProfileTarget::Unknown) {
    return false;
  }
  return validate(encoded, ProfileKey{header.kind, header.target});
}

const ProfileCodec& ProfilePersistence::default_codec() noexcept {
  static const ProfileCodec codec{};
  return codec;
}

ProfileReadStatus ProfilePersistence::load(ProfileKey key,
                                            EncodedProfile& encoded) const noexcept {
  ProfileActivationMetadata metadata{};
  const auto status = store_.read_activation(key, metadata);
  if (status == ProfileReadStatus::Error || status == ProfileReadStatus::Invalid) {
    // A valid slot without a valid committed activation record is only a
    // staged candidate. Never promote it during boot recovery.
    return status;
  }
  if (status != ProfileReadStatus::Found) return status;
  if (!metadata.has_active) return ProfileReadStatus::NotFound;
  auto active_status = store_.read_slot(key, metadata.active_slot, encoded);
  if (active_status == ProfileReadStatus::Found &&
      codec_.validate(encoded, key) && encoded.hash() == metadata.active_hash) {
    return ProfileReadStatus::Found;
  }
  if (!metadata.has_previous) {
    return active_status == ProfileReadStatus::Error ? ProfileReadStatus::Error
                                                     : ProfileReadStatus::Invalid;
  }
  active_status = store_.read_slot(key, metadata.previous_slot, encoded);
  if (active_status == ProfileReadStatus::Found &&
      codec_.validate(encoded, key) && encoded.hash() == metadata.previous_hash) {
    return ProfileReadStatus::Found;
  }
  return active_status == ProfileReadStatus::Error ? ProfileReadStatus::Error
                                                   : ProfileReadStatus::Invalid;
}

ProfileCommitResult ProfilePersistence::commit(ProfileKey key,
                                               const EncodedProfile& encoded) noexcept {
  if (!codec_.validate(encoded, key)) return ProfileCommitResult::Invalid;

  ProfileActivationMetadata old{};
  const auto metadata_status = store_.read_activation(key, old);
  if (metadata_status == ProfileReadStatus::Error ||
      metadata_status == ProfileReadStatus::Invalid) {
    return ProfileCommitResult::ActivationFailed;
  }

  EncodedProfile current{};
  bool have_current = false;
  ProfileSlot destination = ProfileSlot::A;
  if (metadata_status == ProfileReadStatus::Found && old.has_active) {
    EncodedProfile active{};
    const auto active_status = store_.read_slot(key, old.active_slot, active);
    if (active_status == ProfileReadStatus::Found && codec_.validate(active, key) &&
        active.hash() == old.active_hash) {
      current = active;
      have_current = true;
      destination = other_profile_slot(old.active_slot);
    } else if (old.has_previous) {
      const auto previous_status = store_.read_slot(key, old.previous_slot, current);
      if (previous_status == ProfileReadStatus::Found && codec_.validate(current, key) &&
          current.hash() == old.previous_hash) {
        have_current = true;
        destination = old.active_slot;
      }
    } else {
      destination = old.active_slot;
    }
  }

  ParsedHeader incoming{};
  if (!parse_header(encoded, incoming)) return ProfileCommitResult::Invalid;
  if (have_current) {
    ParsedHeader previous{};
    if (!parse_header(current, previous) || incoming.version <= previous.version) {
      return ProfileCommitResult::Invalid;
    }
  }

  if (!store_.write_slot(key, destination, encoded)) return ProfileCommitResult::WriteFailed;
  EncodedProfile readback{};
  const auto read_status = store_.read_slot(key, destination, readback);
  if (read_status != ProfileReadStatus::Found) return ProfileCommitResult::ReadbackFailed;
  if (!codec_.validate(readback, key) || readback.hash() != encoded.hash()) {
    return ProfileCommitResult::IntegrityFailed;
  }

  ProfileActivationMetadata next{};
  next.generation = metadata_status == ProfileReadStatus::Found
                        ? old.generation + (old.generation == UINT32_MAX ? 0U : 1U)
                        : 1U;
  if (metadata_status == ProfileReadStatus::Found && old.generation == UINT32_MAX) {
    return ProfileCommitResult::ActivationFailed;
  }
  next.has_active = true;
  next.active_slot = destination;
  next.active_version = incoming.version;
  next.active_hash = encoded.hash();
  if (have_current) {
    ParsedHeader previous{};
    parse_header(current, previous);
    next.has_previous = true;
    next.previous_slot = destination == ProfileSlot::A ? ProfileSlot::B : ProfileSlot::A;
    if (metadata_status == ProfileReadStatus::Found && old.has_active &&
        destination == old.active_slot) {
      next.previous_slot = old.previous_slot;
    }
    next.previous_version = previous.version;
    next.previous_hash = current.hash();
  }
  if (!store_.write_activation(key, next)) return ProfileCommitResult::ActivationFailed;
  return ProfileCommitResult::Committed;
}

ProfileReadStatus ProfilePersistence::rollback(ProfileKey key) noexcept {
  ProfileActivationMetadata metadata{};
  if (store_.read_activation(key, metadata) != ProfileReadStatus::Found ||
      !metadata.has_active || !metadata.has_previous) {
    return ProfileReadStatus::NotFound;
  }
  EncodedProfile previous{};
  if (store_.read_slot(key, metadata.previous_slot, previous) != ProfileReadStatus::Found ||
      !codec_.validate(previous, key) || previous.hash() != metadata.previous_hash) {
    return ProfileReadStatus::Invalid;
  }
  ProfileActivationMetadata next{};
  next.has_active = true;
  next.active_slot = metadata.previous_slot;
  next.active_version = metadata.previous_version;
  next.active_hash = metadata.previous_hash;
  next.has_previous = true;
  next.previous_slot = metadata.active_slot;
  next.previous_version = metadata.active_version;
  next.previous_hash = metadata.active_hash;
  if (!store_.write_activation(key, next)) return ProfileReadStatus::Error;
  return ProfileReadStatus::Found;
}

bool ProfilePersistence::reset(ProfileKey key) noexcept {
  return store_.write_activation(key, ProfileActivationMetadata{});
}

#ifdef ARDUINO
namespace {
constexpr const char* kProfileNamespace{"sigap_profile"};

void profile_key(char* output, std::size_t capacity, ProfileKey key,
                 ProfileSlot slot) noexcept {
  const char family = key.kind == ProfileKind::Adc ? 'a' :
                      key.kind == ProfileKind::Heater ? 'h' :
                      key.kind == ProfileKind::Capacity ? 'p' : 'c';
  std::snprintf(output, capacity, "%c_%c%02u",
                slot == ProfileSlot::A ? 'a' : 'b', family,
                static_cast<unsigned>(key.target));
}

void metadata_key(char* output, std::size_t capacity, ProfileKey key,
                  std::size_t copy) noexcept {
  const char family = key.kind == ProfileKind::Adc ? 'a' :
                      key.kind == ProfileKind::Heater ? 'h' :
                      key.kind == ProfileKind::Capacity ? 'p' : 'c';
  std::snprintf(output, capacity, "m_%c%02u_%u", family,
                static_cast<unsigned>(key.target),
                static_cast<unsigned>(copy));
}

void put_u32(uint8_t* bytes, std::size_t offset, uint32_t value) noexcept {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1U] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2U] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

uint32_t get_u32(const uint8_t* bytes, std::size_t offset) noexcept {
  return static_cast<uint32_t>(bytes[offset]) |
         static_cast<uint32_t>(bytes[offset + 1U]) << 8U |
         static_cast<uint32_t>(bytes[offset + 2U]) << 16U |
         static_cast<uint32_t>(bytes[offset + 3U]) << 24U;
}

void encode_metadata(const ProfileActivationMetadata& metadata,
                     std::array<uint8_t, kActivationMetadataBytes>& bytes) noexcept {
  bytes.fill(0U);
  bytes[0] = kActivationMetadataFormatVersion;
  put_u32(bytes.data(), kActivationMetadataGenerationOffset, metadata.generation);
  bytes[kActivationMetadataHasActiveOffset] = metadata.has_active ? 1U : 0U;
  bytes[kActivationMetadataActiveSlotOffset] = static_cast<uint8_t>(metadata.active_slot);
  put_u32(bytes.data(), kActivationMetadataActiveVersionOffset, metadata.active_version);
  std::copy(metadata.active_hash.begin(), metadata.active_hash.end(),
            bytes.begin() + kActivationMetadataActiveHashOffset);
  bytes[kActivationMetadataHasPreviousOffset] = metadata.has_previous ? 1U : 0U;
  bytes[kActivationMetadataPreviousSlotOffset] = static_cast<uint8_t>(metadata.previous_slot);
  put_u32(bytes.data(), kActivationMetadataPreviousVersionOffset,
          metadata.previous_version);
  std::copy(metadata.previous_hash.begin(), metadata.previous_hash.end(),
            bytes.begin() + kActivationMetadataPreviousHashOffset);
  const auto integrity = metadata_integrity(metadata);
  std::copy(integrity.begin(), integrity.end(),
            bytes.begin() + kActivationMetadataIntegrityOffset);
}

bool decode_metadata(const uint8_t* bytes, std::size_t size,
                     ProfileActivationMetadata& metadata) noexcept {
  if (bytes == nullptr || size != kActivationMetadataBytes ||
      bytes[0] != kActivationMetadataFormatVersion ||
      bytes[kActivationMetadataActiveSlotOffset] > static_cast<uint8_t>(ProfileSlot::B) ||
      bytes[kActivationMetadataPreviousSlotOffset] > static_cast<uint8_t>(ProfileSlot::B)) {
    return false;
  }
  metadata = ProfileActivationMetadata{};
  metadata.generation = get_u32(bytes, kActivationMetadataGenerationOffset);
  metadata.has_active = bytes[kActivationMetadataHasActiveOffset] != 0U;
  metadata.active_slot = static_cast<ProfileSlot>(bytes[kActivationMetadataActiveSlotOffset]);
  metadata.active_version = get_u32(bytes, kActivationMetadataActiveVersionOffset);
  std::copy_n(bytes + kActivationMetadataActiveHashOffset, metadata.active_hash.size(),
              metadata.active_hash.begin());
  metadata.has_previous = bytes[kActivationMetadataHasPreviousOffset] != 0U;
  metadata.previous_slot = static_cast<ProfileSlot>(bytes[kActivationMetadataPreviousSlotOffset]);
  metadata.previous_version = get_u32(bytes, kActivationMetadataPreviousVersionOffset);
  std::copy_n(bytes + kActivationMetadataPreviousHashOffset, metadata.previous_hash.size(),
              metadata.previous_hash.begin());
  std::copy_n(bytes + kActivationMetadataIntegrityOffset, metadata.integrity.size(),
              metadata.integrity.begin());
  return metadata_integrity_valid(metadata);
}
}

ProfileReadStatus NvsProfileStore::read_activation(
    ProfileTarget target, ProfileActivationMetadata& metadata) const noexcept {
  return read_activation(default_profile_key(target), metadata);
}

ProfileReadStatus NvsProfileStore::read_activation(
    ProfileKey profile_key_id, ProfileActivationMetadata& metadata) const noexcept {
  nvs_handle_t handle{};
  const esp_err_t opened = nvs_open(kProfileNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return ProfileReadStatus::NotFound;
  if (opened != ESP_OK) return ProfileReadStatus::Error;
  ProfileActivationMetadata selected{};
  bool found = false;
  bool invalid = false;
  for (std::size_t copy = 0U; copy < kActivationMetadataCopyCount; ++copy) {
    char key[16]{};
    metadata_key(key, sizeof(key), profile_key_id, copy);
    std::array<uint8_t, kActivationMetadataBytes> bytes{};
    std::size_t size = bytes.size();
    const esp_err_t result = nvs_get_blob(handle, key, bytes.data(), &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) continue;
    if (result != ESP_OK) {
      nvs_close(handle);
      return ProfileReadStatus::Error;
    }
    ProfileActivationMetadata candidate{};
    if (!decode_metadata(bytes.data(), size, candidate)) {
      invalid = true;
      continue;
    }
    if (!found || candidate.generation > selected.generation) {
      selected = candidate;
      found = true;
    }
  }
  nvs_close(handle);
  if (found) {
    metadata = selected;
    return ProfileReadStatus::Found;
  }
  return invalid ? ProfileReadStatus::Invalid : ProfileReadStatus::NotFound;
}

ProfileReadStatus NvsProfileStore::read_slot(
    ProfileTarget target, ProfileSlot slot, EncodedProfile& encoded) const noexcept {
  return read_slot(default_profile_key(target), slot, encoded);
}

ProfileReadStatus NvsProfileStore::read_slot(
    ProfileKey profile_key_id, ProfileSlot slot, EncodedProfile& encoded) const noexcept {
  nvs_handle_t handle{};
  const esp_err_t opened = nvs_open(kProfileNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return ProfileReadStatus::NotFound;
  if (opened != ESP_OK) return ProfileReadStatus::Error;
  char key[12]{};
  profile_key(key, sizeof(key), profile_key_id, slot);
  std::size_t size = encoded.bytes.size();
  const esp_err_t result = nvs_get_blob(handle, key, encoded.bytes.data(), &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return ProfileReadStatus::NotFound;
  if (result != ESP_OK) return ProfileReadStatus::Error;
  if (size > encoded.bytes.size()) return ProfileReadStatus::Invalid;
  encoded.size = size;
  return ProfileReadStatus::Found;
}

bool NvsProfileStore::write_slot(ProfileTarget target, ProfileSlot slot,
                                  const EncodedProfile& encoded) noexcept {
  return write_slot(default_profile_key(target), slot, encoded);
}

bool NvsProfileStore::write_slot(ProfileKey profile_key_id, ProfileSlot slot,
                                  const EncodedProfile& encoded) noexcept {
  if (encoded.size == 0U || encoded.size > encoded.bytes.size()) return false;
  nvs_handle_t handle{};
  if (nvs_open(kProfileNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  char key[12]{};
  profile_key(key, sizeof(key), profile_key_id, slot);
  const esp_err_t result = nvs_set_blob(handle, key, encoded.bytes.data(), encoded.size);
  const esp_err_t committed = result == ESP_OK ? nvs_commit(handle) : result;
  nvs_close(handle);
  return committed == ESP_OK;
}

bool NvsProfileStore::write_activation(
    ProfileTarget target, const ProfileActivationMetadata& metadata) noexcept {
  return write_activation(default_profile_key(target), metadata);
}

bool NvsProfileStore::write_activation(
    ProfileKey profile_key_id, const ProfileActivationMetadata& metadata) noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kProfileNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  // Write a new metadata copy while retaining the previous committed copy.
  std::size_t destination = 0U;
  uint32_t generation = metadata.generation;
  uint32_t newest_generation = 0U;
  std::size_t newest_copy = 0U;
  bool have_existing = false;
  for (std::size_t copy = 0U; copy < kActivationMetadataCopyCount; ++copy) {
    char existing_key[16]{};
    metadata_key(existing_key, sizeof(existing_key), profile_key_id, copy);
    std::array<uint8_t, kActivationMetadataBytes> existing_bytes{};
    std::size_t size = existing_bytes.size();
    const esp_err_t result = nvs_get_blob(handle, existing_key, existing_bytes.data(), &size);
    if (result == ESP_OK) {
      ProfileActivationMetadata existing{};
      if (decode_metadata(existing_bytes.data(), size, existing)) {
        if (!have_existing || existing.generation > newest_generation) {
          newest_generation = existing.generation;
          newest_copy = copy;
          have_existing = true;
        }
      }
    } else if (result != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(handle);
      return false;
    }
  }
  if (generation == UINT32_MAX) {
    nvs_close(handle);
    return false;
  }
  if (have_existing) {
    generation = std::max(generation, newest_generation);
    if (generation == UINT32_MAX) {
      nvs_close(handle);
      return false;
    }
    destination = newest_copy == 0U ? 1U : 0U;
  }
  ProfileActivationMetadata to_write = metadata;
  to_write.generation = generation + 1U;
  char key[16]{};
  metadata_key(key, sizeof(key), profile_key_id, destination);
  std::array<uint8_t, kActivationMetadataBytes> bytes{};
  encode_metadata(to_write, bytes);
  const esp_err_t result = nvs_set_blob(handle, key, bytes.data(), bytes.size());
  const esp_err_t committed = result == ESP_OK ? nvs_commit(handle) : result;
  nvs_close(handle);
  return committed == ESP_OK;
}
#else
ProfileReadStatus NvsProfileStore::read_activation(
    ProfileTarget, ProfileActivationMetadata&) const noexcept {
  return ProfileReadStatus::Error;
}
ProfileReadStatus NvsProfileStore::read_activation(
    ProfileKey, ProfileActivationMetadata&) const noexcept {
  return ProfileReadStatus::Error;
}
ProfileReadStatus NvsProfileStore::read_slot(
    ProfileTarget, ProfileSlot, EncodedProfile&) const noexcept {
  return ProfileReadStatus::Error;
}
ProfileReadStatus NvsProfileStore::read_slot(
    ProfileKey, ProfileSlot, EncodedProfile&) const noexcept {
  return ProfileReadStatus::Error;
}
bool NvsProfileStore::write_slot(ProfileTarget, ProfileSlot,
                                 const EncodedProfile&) noexcept { return false; }
bool NvsProfileStore::write_slot(ProfileKey, ProfileSlot,
                                 const EncodedProfile&) noexcept { return false; }
bool NvsProfileStore::write_activation(
    ProfileTarget, const ProfileActivationMetadata&) noexcept { return false; }
bool NvsProfileStore::write_activation(
    ProfileKey, const ProfileActivationMetadata&) noexcept { return false; }
#endif

std::size_t MemoryProfileStore::target_index(ProfileTarget target) noexcept {
  const auto value = static_cast<uint32_t>(target);
  return value < kTargetCount ? static_cast<std::size_t>(value) : 0U;
}

std::size_t MemoryProfileStore::key_index(ProfileKey key) noexcept {
  const auto target = static_cast<uint32_t>(key.target);
  if (key.kind == ProfileKind::Calibration) {
    if (target >= 1U && target <= 6U) return target - 1U;
    if (target == 11U) return 6U;
    if (target == 12U) return 7U;
    if (target == 13U) return 8U;
  } else if (key.kind == ProfileKind::Adc && target >= 7U && target <= 10U) {
    return 9U + target - 7U;
  } else if (key.kind == ProfileKind::Heater && key.target == ProfileTarget::Mq7) {
    return 13U;
  } else if (key.kind == ProfileKind::Capacity && target >= 1U && target <= 3U) {
    return 14U + target - 1U;
  }
  return 0U;
}

ProfileReadStatus MemoryProfileStore::read_activation(
    ProfileTarget target, ProfileActivationMetadata& metadata) const noexcept {
  return read_activation(default_profile_key(target), metadata);
}

ProfileReadStatus MemoryProfileStore::read_activation(
    ProfileKey key, ProfileActivationMetadata& metadata) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadActivation) {
    has_pending_fault_ = false;
    return ProfileReadStatus::Error;
  }
  const auto index = key_index(key);
  bool found = false;
  bool invalid = false;
  ProfileActivationMetadata selected{};
  for (std::size_t copy = 0U; copy < kActivationMetadataCopyCount; ++copy) {
    if (!metadata_present_[index][copy]) continue;
    if (!metadata_integrity_valid(metadata_[index][copy])) {
      invalid = true;
      continue;
    }
    if (!found || metadata_[index][copy].generation > selected.generation) {
      selected = metadata_[index][copy];
      found = true;
    }
  }
  if (found) {
    metadata = selected;
    return ProfileReadStatus::Found;
  }
  return invalid ? ProfileReadStatus::Invalid : ProfileReadStatus::NotFound;
}

ProfileReadStatus MemoryProfileStore::read_slot(ProfileTarget target, ProfileSlot slot,
                                                EncodedProfile& encoded) const noexcept {
  return read_slot(default_profile_key(target), slot, encoded);
}

ProfileReadStatus MemoryProfileStore::read_slot(ProfileKey key, ProfileSlot slot,
                                                EncodedProfile& encoded) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadSlot) {
    has_pending_fault_ = false;
    return ProfileReadStatus::Error;
  }
  const auto target_i = key_index(key);
  const auto slot_i = slot_index(slot);
  if (!present_[target_i][slot_i]) return ProfileReadStatus::NotFound;
  encoded = slots_[target_i][slot_i];
  return ProfileReadStatus::Found;
}

bool MemoryProfileStore::write_slot(ProfileTarget target, ProfileSlot slot,
                                    const EncodedProfile& encoded) noexcept {
  return write_slot(default_profile_key(target), slot, encoded);
}

bool MemoryProfileStore::write_slot(ProfileKey key, ProfileSlot slot,
                                    const EncodedProfile& encoded) noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::WriteSlot) {
    has_pending_fault_ = false;
    return false;
  }
  const auto target_i = key_index(key);
  const auto slot_i = slot_index(slot);
  slots_[target_i][slot_i] = encoded;
  present_[target_i][slot_i] = true;
  if (has_pending_fault_ && pending_fault_ == Fault::CorruptAfterWrite) {
    slots_[target_i][slot_i].bytes[0] ^= 0xffU;
    has_pending_fault_ = false;
  }
  return true;
}

bool MemoryProfileStore::write_activation(
    ProfileTarget target, const ProfileActivationMetadata& metadata) noexcept {
  return write_activation(default_profile_key(target), metadata);
}

bool MemoryProfileStore::write_activation(
    ProfileKey key, const ProfileActivationMetadata& metadata) noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::WriteActivation) {
    has_pending_fault_ = false;
    return false;
  }
  const auto index = key_index(key);
  std::size_t destination = 0U;
  uint32_t generation = metadata.generation;
  bool found = false;
  uint32_t newest_generation = 0U;
  std::size_t newest_copy = 0U;
  for (std::size_t copy = 0U; copy < kActivationMetadataCopyCount; ++copy) {
    if (!metadata_present_[index][copy] || !metadata_integrity_valid(metadata_[index][copy])) {
      continue;
    }
    found = true;
    if (!found || metadata_[index][copy].generation > newest_generation) {
      newest_generation = metadata_[index][copy].generation;
      newest_copy = copy;
    }
  }
  if (found) {
    generation = std::max(generation, newest_generation);
    if (generation == UINT32_MAX) return false;
    destination = newest_copy == 0U ? 1U : 0U;
  }
  metadata_[index][destination] = metadata;
  metadata_[index][destination].generation = found ? generation + 1U : 1U;
  metadata_[index][destination].integrity = metadata_integrity(metadata_[index][destination]);
  metadata_present_[index][destination] = true;
  return true;
}

void MemoryProfileStore::fail_next(Fault fault) noexcept {
  pending_fault_ = fault;
  has_pending_fault_ = true;
}

void MemoryProfileStore::corrupt_slot(ProfileTarget target, ProfileSlot slot) noexcept {
  const auto target_i = key_index(default_profile_key(target));
  const auto slot_i = slot_index(slot);
  if (present_[target_i][slot_i]) slots_[target_i][slot_i].bytes[0] ^= 0xffU;
}

bool MemoryProfileStore::slot_present(ProfileTarget target, ProfileSlot slot) const noexcept {
  return present_[key_index(default_profile_key(target))][slot_index(slot)];
}

void MemoryProfileStore::corrupt_activation(ProfileKey key) noexcept {
  const auto index = key_index(key);
  for (std::size_t copy = 0U; copy < kActivationMetadataCopyCount; ++copy) {
    if (metadata_present_[index][copy]) metadata_[index][copy].active_hash[0] ^= 0xffU;
  }
}

void MemoryProfileStore::corrupt_activation_copy(ProfileKey key,
                                                  std::size_t copy) noexcept {
  const auto index = key_index(key);
  if (copy < kActivationMetadataCopyCount && metadata_present_[index][copy]) {
    metadata_[index][copy].active_hash[0] ^= 0xffU;
  }
}

}  // namespace sigap::acquisition
