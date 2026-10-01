#include "acquisition/profile_store.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <cstdio>

#include "../../../telemetry_contract/src/telemetry/sha256.h"

#ifdef ARDUINO
#include <nvs.h>
#endif

namespace sigap::acquisition {
namespace {

using telemetry_contract::sha256;

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
  std::size_t offset() const noexcept { return offset_; }

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
  ProfileTarget target{ProfileTarget::Unknown};
  uint32_t version{0U};
  ProfileValidity validity{ProfileValidity::Missing};
  uint16_t payload_length{0U};
  std::size_t payload_offset{kProfileHeaderBytes};
};

bool parse_header(const EncodedProfile& encoded, ParsedHeader& header) noexcept {
  if (encoded.size < kProfileHeaderBytes + kProfileHashBytes ||
      encoded.size > encoded.bytes.size()) {
    return false;
  }
  Reader reader(encoded.bytes.data(), encoded.size);
  uint32_t magic{0U};
  uint16_t format{0U};
  uint32_t target{0U};
  uint8_t validity{0U};
  if (!reader.u32(magic) || !reader.u16(format) || !reader.u32(header.schema) ||
      !reader.u32(target) || !reader.u32(header.version) ||
      !reader.u8(validity) || !reader.u16(header.payload_length)) {
    return false;
  }
  if (magic != kProfileMagic || format != kProfileFormatVersion ||
      header.schema != kProfileSchemaVersion ||
      header.payload_length > kProfilePayloadBytes ||
      encoded.size != kProfileHeaderBytes + header.payload_length + kProfileHashBytes) {
    return false;
  }
  header.target = static_cast<ProfileTarget>(target);
  header.validity = static_cast<ProfileValidity>(validity);
  return header.validity <= ProfileValidity::TestOnly;
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
      !writer.u8(static_cast<uint8_t>(manifest.validity)) ||
      !writer.u16(0U)) {
    return false;
  }
  return true;
}

void finish_encoded(EncodedProfile& output, std::size_t payload_end) noexcept {
  const std::size_t payload_length = payload_end - kProfileHeaderBytes;
  output.bytes[19U] = static_cast<uint8_t>(payload_length);
  output.bytes[20U] = static_cast<uint8_t>(payload_length >> 8U);
  const auto digest = sha256(std::string_view(
      reinterpret_cast<const char*>(output.bytes.data()), payload_end));
  std::copy(digest.begin(), digest.end(), output.bytes.begin() + payload_end);
  output.size = payload_end + kProfileHashBytes;
}

ProfileManifest manifest_from(const ParsedHeader& header,
                              const config::ConfigHash& hash) noexcept {
  ProfileManifest manifest{};
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
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown, output, writer) ||
      !writer.f32(profile.counts_per_kg) || !writer.i64(profile.tare_offset_counts)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const UltrasonicProfile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown, output, writer) ||
      !writer.f32(profile.d_empty_mm) || !writer.f32(profile.d_full_mm) ||
      !writer.f32(profile.echo_mm_per_us) ||
      !writer.f32(profile.overshoot_tolerance_mm) ||
      !writer.u32(profile.echo_timeout_us) ||
      !writer.u32(profile.inter_sensor_gap_us)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const MqAdcProfile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown, output, writer) ||
      !writer.u32(profile.reference_mv) || !writer.u32(profile.full_scale_counts) ||
      !writer.u32(profile.divider_numerator) ||
      !writer.u32(profile.divider_denominator)) {
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
  Writer writer(output.bytes);
  if (!begin_payload(manifest, ProfileTarget::Mq7, output, writer) ||
      !writer.u64(profile.preheat_us) || !writer.u64(profile.high_clean_us) ||
      !writer.u64(profile.low_detect_us)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const Ds18b20Profile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown, output, writer) ||
      !writer.u8(profile.has_rom ? 1U : 0U) || !writer.bytes(profile.rom.data(), profile.rom.size()) ||
      !writer.u8(profile.resolution_bits) || !writer.i32(profile.offset_centi_c)) {
    return false;
  }
  finish_encoded(output, writer.offset());
  return true;
}

bool ProfileCodec::encode(const Sht31Profile& profile,
                          EncodedProfile& output) const noexcept {
  Writer writer(output.bytes);
  if (!begin_payload(profile.manifest, ProfileTarget::Unknown, output, writer) ||
      !writer.u8(profile.address) || !writer.i32(profile.temperature_offset_centi_c) ||
      !writer.i32(profile.humidity_offset_centi_pct)) {
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
  if (h.target < ProfileTarget::Hx711Organic || h.target > ProfileTarget::Hx711B3) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.f32(output.counts_per_kg) && reader.i64(output.tare_offset_counts) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          UltrasonicProfile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Unknown, h, output) ||
      h.target < ProfileTarget::UltrasonicOrganic || h.target > ProfileTarget::UltrasonicB3) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.f32(output.d_empty_mm) && reader.f32(output.d_full_mm) &&
         reader.f32(output.echo_mm_per_us) && reader.f32(output.overshoot_tolerance_mm) &&
         reader.u32(output.echo_timeout_us) && reader.u32(output.inter_sensor_gap_us) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          MqAdcProfile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Unknown, h, output) ||
      h.target < ProfileTarget::Mq4 || h.target > ProfileTarget::Mq8) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.u32(output.reference_mv) && reader.u32(output.full_scale_counts) &&
         reader.u32(output.divider_numerator) && reader.u32(output.divider_denominator) &&
         reader.offset() == h.payload_length;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          Mq7HeaterProfile& output) const noexcept {
  ParsedHeader h{};
  if (!parse_header(encoded, h) || !hash_matches(encoded) ||
      h.target != ProfileTarget::Mq7) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  if (!reader.u64(output.preheat_us) || !reader.u64(output.high_clean_us) ||
      !reader.u64(output.low_detect_us) || reader.offset() != h.payload_length) {
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
  if (!decode_header(encoded, ProfileTarget::Ds18b20B3, h, output)) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  uint8_t has_rom{0U};
  if (!reader.u8(has_rom) || !reader.bytes(output.rom.data(), output.rom.size()) ||
      !reader.u8(output.resolution_bits) || !reader.i32(output.offset_centi_c) ||
      reader.offset() != h.payload_length) return false;
  output.has_rom = has_rom != 0U;
  return true;
}

bool ProfileCodec::decode(const EncodedProfile& encoded,
                          Sht31Profile& output) const noexcept {
  ParsedHeader h{};
  if (!decode_header(encoded, ProfileTarget::Unknown, h, output) ||
      (h.target != ProfileTarget::Sht31_44 && h.target != ProfileTarget::Sht31_45)) return false;
  Reader reader(encoded.bytes.data() + h.payload_offset, h.payload_length);
  return reader.u8(output.address) && reader.i32(output.temperature_offset_centi_c) &&
         reader.i32(output.humidity_offset_centi_pct) && reader.offset() == h.payload_length;
}

bool ProfileCodec::validate(const EncodedProfile& encoded,
                            ProfileTarget expected) const noexcept {
  ParsedHeader header{};
  return parse_header(encoded, header) && hash_matches(encoded) &&
         valid_target(header.target, expected) && header.version != 0U;
}

const ProfileCodec& ProfilePersistence::default_codec() noexcept {
  static const ProfileCodec codec{};
  return codec;
}

ProfileReadStatus ProfilePersistence::load(ProfileTarget target,
                                            EncodedProfile& encoded) const noexcept {
  ProfileActivationMetadata metadata{};
  const auto status = store_.read_activation(target, metadata);
  if (status != ProfileReadStatus::Found) return status;
  if (!metadata.has_active) return ProfileReadStatus::NotFound;
  auto active_status = store_.read_slot(target, metadata.active_slot, encoded);
  if (active_status == ProfileReadStatus::Found &&
      codec_.validate(encoded, target) && encoded.hash() == metadata.active_hash) {
    return ProfileReadStatus::Found;
  }
  if (!metadata.has_previous) {
    return active_status == ProfileReadStatus::Error ? ProfileReadStatus::Error
                                                     : ProfileReadStatus::Invalid;
  }
  active_status = store_.read_slot(target, metadata.previous_slot, encoded);
  if (active_status == ProfileReadStatus::Found &&
      codec_.validate(encoded, target) && encoded.hash() == metadata.previous_hash) {
    return ProfileReadStatus::Found;
  }
  return active_status == ProfileReadStatus::Error ? ProfileReadStatus::Error
                                                   : ProfileReadStatus::Invalid;
}

ProfileCommitResult ProfilePersistence::commit(ProfileTarget target,
                                               const EncodedProfile& encoded) noexcept {
  if (!codec_.validate(encoded, target)) return ProfileCommitResult::Invalid;

  ProfileActivationMetadata old{};
  const auto metadata_status = store_.read_activation(target, old);
  if (metadata_status == ProfileReadStatus::Error) {
    return ProfileCommitResult::ActivationFailed;
  }

  EncodedProfile current{};
  bool have_current = false;
  ProfileSlot destination = ProfileSlot::A;
  if (metadata_status == ProfileReadStatus::Found && old.has_active) {
    EncodedProfile active{};
    const auto active_status = store_.read_slot(target, old.active_slot, active);
    if (active_status == ProfileReadStatus::Found && codec_.validate(active, target) &&
        active.hash() == old.active_hash) {
      current = active;
      have_current = true;
      destination = other_profile_slot(old.active_slot);
    } else if (old.has_previous) {
      const auto previous_status = store_.read_slot(target, old.previous_slot, current);
      if (previous_status == ProfileReadStatus::Found && codec_.validate(current, target) &&
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

  if (!store_.write_slot(target, destination, encoded)) return ProfileCommitResult::WriteFailed;
  EncodedProfile readback{};
  const auto read_status = store_.read_slot(target, destination, readback);
  if (read_status != ProfileReadStatus::Found) return ProfileCommitResult::ReadbackFailed;
  if (!codec_.validate(readback, target) || readback.hash() != encoded.hash()) {
    return ProfileCommitResult::IntegrityFailed;
  }

  ProfileActivationMetadata next{};
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
  if (!store_.write_activation(target, next)) return ProfileCommitResult::ActivationFailed;
  return ProfileCommitResult::Committed;
}

ProfileReadStatus ProfilePersistence::rollback(ProfileTarget target) noexcept {
  ProfileActivationMetadata metadata{};
  if (store_.read_activation(target, metadata) != ProfileReadStatus::Found ||
      !metadata.has_active || !metadata.has_previous) {
    return ProfileReadStatus::NotFound;
  }
  EncodedProfile previous{};
  if (store_.read_slot(target, metadata.previous_slot, previous) != ProfileReadStatus::Found ||
      !codec_.validate(previous, target) || previous.hash() != metadata.previous_hash) {
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
  if (!store_.write_activation(target, next)) return ProfileReadStatus::Error;
  return ProfileReadStatus::Found;
}

bool ProfilePersistence::reset(ProfileTarget target) noexcept {
  return store_.write_activation(target, ProfileActivationMetadata{});
}

#ifdef ARDUINO
namespace {
constexpr const char* kProfileNamespace{"sigap_profile"};
constexpr std::size_t kMetadataBytes{76U};

void profile_key(char* output, std::size_t capacity, ProfileTarget target,
                 ProfileSlot slot) noexcept {
  std::snprintf(output, capacity, "%c%u", slot == ProfileSlot::A ? 'a' : 'b',
                static_cast<unsigned>(target));
}

void metadata_key(char* output, std::size_t capacity, ProfileTarget target) noexcept {
  std::snprintf(output, capacity, "m%u", static_cast<unsigned>(target));
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
                     std::array<uint8_t, kMetadataBytes>& bytes) noexcept {
  bytes.fill(0U);
  bytes[0] = 1U;
  bytes[1] = metadata.has_active ? 1U : 0U;
  bytes[2] = static_cast<uint8_t>(metadata.active_slot);
  put_u32(bytes.data(), 3U, metadata.active_version);
  std::copy(metadata.active_hash.begin(), metadata.active_hash.end(), bytes.begin() + 7U);
  bytes[39] = metadata.has_previous ? 1U : 0U;
  bytes[40] = static_cast<uint8_t>(metadata.previous_slot);
  put_u32(bytes.data(), 41U, metadata.previous_version);
  std::copy(metadata.previous_hash.begin(), metadata.previous_hash.end(), bytes.begin() + 45U);
}

bool decode_metadata(const uint8_t* bytes, std::size_t size,
                     ProfileActivationMetadata& metadata) noexcept {
  if (bytes == nullptr || size != kMetadataBytes || bytes[0] != 1U ||
      bytes[2] > static_cast<uint8_t>(ProfileSlot::B) ||
      bytes[40] > static_cast<uint8_t>(ProfileSlot::B)) return false;
  metadata = ProfileActivationMetadata{};
  metadata.has_active = bytes[1] != 0U;
  metadata.active_slot = static_cast<ProfileSlot>(bytes[2]);
  metadata.active_version = get_u32(bytes, 3U);
  std::copy_n(bytes + 7U, metadata.active_hash.size(), metadata.active_hash.begin());
  metadata.has_previous = bytes[39] != 0U;
  metadata.previous_slot = static_cast<ProfileSlot>(bytes[40]);
  metadata.previous_version = get_u32(bytes, 41U);
  std::copy_n(bytes + 45U, metadata.previous_hash.size(), metadata.previous_hash.begin());
  return true;
}
}

ProfileReadStatus NvsProfileStore::read_activation(
    ProfileTarget target, ProfileActivationMetadata& metadata) const noexcept {
  nvs_handle_t handle{};
  const esp_err_t opened = nvs_open(kProfileNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return ProfileReadStatus::NotFound;
  if (opened != ESP_OK) return ProfileReadStatus::Error;
  char key[12]{};
  metadata_key(key, sizeof(key), target);
  std::array<uint8_t, kMetadataBytes> bytes{};
  std::size_t size = bytes.size();
  const esp_err_t result = nvs_get_blob(handle, key, bytes.data(), &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return ProfileReadStatus::NotFound;
  if (result != ESP_OK) return ProfileReadStatus::Error;
  return decode_metadata(bytes.data(), size, metadata) ? ProfileReadStatus::Found
                                                       : ProfileReadStatus::Invalid;
}

ProfileReadStatus NvsProfileStore::read_slot(
    ProfileTarget target, ProfileSlot slot, EncodedProfile& encoded) const noexcept {
  nvs_handle_t handle{};
  const esp_err_t opened = nvs_open(kProfileNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return ProfileReadStatus::NotFound;
  if (opened != ESP_OK) return ProfileReadStatus::Error;
  char key[12]{};
  profile_key(key, sizeof(key), target, slot);
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
  if (encoded.size == 0U || encoded.size > encoded.bytes.size()) return false;
  nvs_handle_t handle{};
  if (nvs_open(kProfileNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  char key[12]{};
  profile_key(key, sizeof(key), target, slot);
  const esp_err_t result = nvs_set_blob(handle, key, encoded.bytes.data(), encoded.size);
  const esp_err_t committed = result == ESP_OK ? nvs_commit(handle) : result;
  nvs_close(handle);
  return committed == ESP_OK;
}

bool NvsProfileStore::write_activation(
    ProfileTarget target, const ProfileActivationMetadata& metadata) noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kProfileNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  char key[12]{};
  metadata_key(key, sizeof(key), target);
  std::array<uint8_t, kMetadataBytes> bytes{};
  encode_metadata(metadata, bytes);
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
ProfileReadStatus NvsProfileStore::read_slot(
    ProfileTarget, ProfileSlot, EncodedProfile&) const noexcept {
  return ProfileReadStatus::Error;
}
bool NvsProfileStore::write_slot(ProfileTarget, ProfileSlot,
                                 const EncodedProfile&) noexcept { return false; }
bool NvsProfileStore::write_activation(
    ProfileTarget, const ProfileActivationMetadata&) noexcept { return false; }
#endif

std::size_t MemoryProfileStore::target_index(ProfileTarget target) noexcept {
  const auto value = static_cast<uint32_t>(target);
  return value < kTargetCount ? static_cast<std::size_t>(value) : 0U;
}

ProfileReadStatus MemoryProfileStore::read_activation(
    ProfileTarget target, ProfileActivationMetadata& metadata) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadActivation) {
    has_pending_fault_ = false;
    return ProfileReadStatus::Error;
  }
  const auto index = target_index(target);
  if (!metadata_present_[index]) return ProfileReadStatus::NotFound;
  metadata = metadata_[index];
  return ProfileReadStatus::Found;
}

ProfileReadStatus MemoryProfileStore::read_slot(ProfileTarget target, ProfileSlot slot,
                                                EncodedProfile& encoded) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadSlot) {
    has_pending_fault_ = false;
    return ProfileReadStatus::Error;
  }
  const auto target_i = target_index(target);
  const auto slot_i = slot_index(slot);
  if (!present_[target_i][slot_i]) return ProfileReadStatus::NotFound;
  encoded = slots_[target_i][slot_i];
  return ProfileReadStatus::Found;
}

bool MemoryProfileStore::write_slot(ProfileTarget target, ProfileSlot slot,
                                    const EncodedProfile& encoded) noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::WriteSlot) {
    has_pending_fault_ = false;
    return false;
  }
  const auto target_i = target_index(target);
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
  if (has_pending_fault_ && pending_fault_ == Fault::WriteActivation) {
    has_pending_fault_ = false;
    return false;
  }
  const auto index = target_index(target);
  metadata_[index] = metadata;
  metadata_present_[index] = true;
  return true;
}

void MemoryProfileStore::fail_next(Fault fault) noexcept {
  pending_fault_ = fault;
  has_pending_fault_ = true;
}

void MemoryProfileStore::corrupt_slot(ProfileTarget target, ProfileSlot slot) noexcept {
  const auto target_i = target_index(target);
  const auto slot_i = slot_index(slot);
  if (present_[target_i][slot_i]) slots_[target_i][slot_i].bytes[0] ^= 0xffU;
}

bool MemoryProfileStore::slot_present(ProfileTarget target, ProfileSlot slot) const noexcept {
  return present_[target_index(target)][slot_index(slot)];
}

}  // namespace sigap::acquisition
