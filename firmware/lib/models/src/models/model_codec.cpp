#include "models/model_codec.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "telemetry/sha256.h"

namespace sigap::models {
namespace {

using telemetry_contract::sha256;

class Writer final {
 public:
  explicit Writer(EncodedModelProfile& output) noexcept : output_(output) {
    output_ = EncodedModelProfile{};
  }

  bool u8(uint8_t value) noexcept { return bytes(&value, 1U); }
  bool u16(uint16_t value) noexcept {
    const uint8_t encoded[2]{static_cast<uint8_t>(value),
                             static_cast<uint8_t>(value >> 8U)};
    return bytes(encoded, sizeof(encoded));
  }
  bool u32(uint32_t value) noexcept {
    uint8_t encoded[4]{};
    for (std::size_t index = 0U; index < sizeof(encoded); ++index) {
      encoded[index] = static_cast<uint8_t>(value >> (8U * index));
    }
    return bytes(encoded, sizeof(encoded));
  }
  bool u64(uint64_t value) noexcept {
    uint8_t encoded[8]{};
    for (std::size_t index = 0U; index < sizeof(encoded); ++index) {
      encoded[index] = static_cast<uint8_t>(value >> (8U * index));
    }
    return bytes(encoded, sizeof(encoded));
  }
  bool i64(int64_t value) noexcept {
    return u64(static_cast<uint64_t>(value));
  }
  bool f32(float value) noexcept {
    uint32_t bits{0U};
    static_assert(sizeof(bits) == sizeof(value), "float codec width");
    std::memcpy(&bits, &value, sizeof(bits));
    return u32(bits);
  }
  bool id(const ModelId& value) noexcept {
    return bytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }
  bool provenance(const acquisition::ProfileProvenance& value) noexcept {
    return u8(value.present ? 1U : 0U) &&
           u8(value.boot_id.present ? 1U : 0U) &&
           bytes(value.boot_id.bytes.data(), value.boot_id.bytes.size()) &&
           u64(value.monotonic_us) &&
           u8(value.recorded_at.available ? 1U : 0U) &&
           i64(value.recorded_at.epoch_ms) &&
           u8(static_cast<uint8_t>(value.time_quality)) &&
           i64(value.reference_primary) &&
           i64(value.reference_secondary) &&
           u32(value.evidence_count) && u64(value.evidence_sequence) &&
           u8(value.units) &&
           bytes(reinterpret_cast<const uint8_t*>(value.reference_note.data()),
                 value.reference_note.size());
  }
  bool bytes(const uint8_t* source, std::size_t count) noexcept {
    if (source == nullptr || offset_ + count > output_.bytes.size()) return false;
    std::copy_n(source, count, output_.bytes.begin() + offset_);
    offset_ += count;
    return true;
  }
  std::size_t offset() const noexcept { return offset_; }

 private:
  EncodedModelProfile& output_;
  std::size_t offset_{0U};
};

class Reader final {
 public:
  Reader(const uint8_t* bytes, std::size_t size) noexcept
      : bytes_(bytes), size_(size) {}

  bool u8(uint8_t& value) noexcept { return bytes(&value, 1U); }
  bool u16(uint16_t& value) noexcept {
    uint8_t encoded[2]{};
    if (!bytes(encoded, sizeof(encoded))) return false;
    value = static_cast<uint16_t>(encoded[0]) |
            static_cast<uint16_t>(encoded[1]) << 8U;
    return true;
  }
  bool u32(uint32_t& value) noexcept {
    uint8_t encoded[4]{};
    if (!bytes(encoded, sizeof(encoded))) return false;
    value = 0U;
    for (std::size_t index = 0U; index < sizeof(encoded); ++index) {
      value |= static_cast<uint32_t>(encoded[index]) << (8U * index);
    }
    return true;
  }
  bool u64(uint64_t& value) noexcept {
    uint8_t encoded[8]{};
    if (!bytes(encoded, sizeof(encoded))) return false;
    value = 0U;
    for (std::size_t index = 0U; index < sizeof(encoded); ++index) {
      value |= static_cast<uint64_t>(encoded[index]) << (8U * index);
    }
    return true;
  }
  bool i64(int64_t& value) noexcept {
    uint64_t bits{0U};
    if (!u64(bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }
  bool f32(float& value) noexcept {
    uint32_t bits{0U};
    if (!u32(bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }
  bool id(ModelId& value) noexcept {
    return bytes(reinterpret_cast<uint8_t*>(value.data()), value.size());
  }
  bool provenance(acquisition::ProfileProvenance& value) noexcept {
    uint8_t present{0U};
    uint8_t boot_present{0U};
    uint8_t recorded_at{0U};
    uint8_t time_quality{0U};
    if (!u8(present) || !u8(boot_present) ||
        !bytes(value.boot_id.bytes.data(), value.boot_id.bytes.size()) ||
        !u64(value.monotonic_us) || !u8(recorded_at) ||
        !i64(value.recorded_at.epoch_ms) || !u8(time_quality) ||
        !i64(value.reference_primary) || !i64(value.reference_secondary) ||
        !u32(value.evidence_count) || !u64(value.evidence_sequence) ||
        !u8(value.units) ||
        !bytes(reinterpret_cast<uint8_t*>(value.reference_note.data()),
               value.reference_note.size())) {
      return false;
    }
    if (present > 1U || boot_present > 1U || recorded_at > 1U ||
        time_quality >
            static_cast<uint8_t>(acquisition::TimeQuality::Estimated)) {
      return false;
    }
    value.present = present != 0U;
    value.boot_id.present = boot_present != 0U;
    value.recorded_at.available = recorded_at != 0U;
    value.time_quality = static_cast<acquisition::TimeQuality>(time_quality);
    return true;
  }
  bool bytes(uint8_t* destination, std::size_t count) noexcept {
    if (destination == nullptr || bytes_ == nullptr ||
        offset_ + count > size_) return false;
    std::copy_n(bytes_ + offset_, count, destination);
    offset_ += count;
    return true;
  }
  std::size_t remaining() const noexcept { return size_ - offset_; }

 private:
  const uint8_t* bytes_{nullptr};
  std::size_t size_{0U};
  std::size_t offset_{0U};
};

struct Header final {
  uint32_t schema{0U};
  acquisition::ProfileKind kind{acquisition::ProfileKind::Fis};
  ModelTarget target{ModelTarget::Unknown};
  uint32_t version{0U};
  acquisition::ProfileValidity validity{
      acquisition::ProfileValidity::Missing};
  ModelDeployment deployment{ModelDeployment::Unconfigured};
  uint16_t payload_size{0U};
};

bool write_header(Writer& writer, const acquisition::ProfileManifest& manifest,
                  ModelDeployment deployment) noexcept {
  return writer.u32(kModelEnvelopeMagic) &&
         writer.u16(kModelEnvelopeFormatVersion) &&
         writer.u32(manifest.schema_version) &&
         writer.u8(static_cast<uint8_t>(manifest.kind)) &&
         writer.u32(manifest.target_id) &&
         writer.u32(manifest.profile_version) &&
         writer.u8(static_cast<uint8_t>(manifest.validity)) &&
         writer.u8(static_cast<uint8_t>(deployment)) && writer.u16(0U);
}

bool parse_header(const EncodedModelProfile& encoded, Header& header) noexcept {
  if (encoded.size < kModelEnvelopeHeaderBytes + kModelHashBytes ||
      encoded.size > encoded.bytes.size()) return false;
  Reader reader(encoded.bytes.data(), encoded.size);
  uint32_t magic{0U};
  uint16_t format{0U};
  uint8_t kind{0U};
  uint32_t target{0U};
  uint8_t validity{0U};
  uint8_t deployment{0U};
  if (!reader.u32(magic) || !reader.u16(format) ||
      !reader.u32(header.schema) || !reader.u8(kind) ||
      !reader.u32(target) || !reader.u32(header.version) ||
      !reader.u8(validity) || !reader.u8(deployment) ||
      !reader.u16(header.payload_size)) return false;
  if (magic != kModelEnvelopeMagic || format != kModelEnvelopeFormatVersion ||
      header.schema != kModelProfileSchemaVersion || header.version == 0U ||
      header.payload_size > kMaxModelPayloadBytes ||
      encoded.size != kModelEnvelopeHeaderBytes + header.payload_size +
                          kModelHashBytes) return false;
  header.kind = static_cast<acquisition::ProfileKind>(kind);
  header.target = static_cast<ModelTarget>(target);
  header.validity = static_cast<acquisition::ProfileValidity>(validity);
  header.deployment = static_cast<ModelDeployment>(deployment);
  return valid_model_key(ModelProfileKey{header.kind, header.target});
}

bool hash_matches(const EncodedModelProfile& encoded,
                  const Header& header) noexcept {
  const std::size_t hashed_size =
      kModelEnvelopeHeaderBytes + header.payload_size;
  const auto digest = sha256(std::string_view(
      reinterpret_cast<const char*>(encoded.bytes.data()), hashed_size));
  return std::equal(digest.begin(), digest.end(),
                    encoded.bytes.begin() + hashed_size);
}

void finish(EncodedModelProfile& output, std::size_t payload_end) noexcept {
  const auto payload_size = payload_end - kModelEnvelopeHeaderBytes;
  output.bytes[21U] = static_cast<uint8_t>(payload_size);
  output.bytes[22U] = static_cast<uint8_t>(payload_size >> 8U);
  const auto digest = sha256(std::string_view(
      reinterpret_cast<const char*>(output.bytes.data()), payload_end));
  std::copy(digest.begin(), digest.end(), output.bytes.begin() + payload_end);
  output.size = payload_end + digest.size();
}

acquisition::ProfileManifest manifest_from(
    const Header& header,
    const config::ConfigHash& hash) noexcept {
  acquisition::ProfileManifest manifest{};
  manifest.kind = header.kind;
  manifest.schema_version = header.schema;
  manifest.profile_version = header.version;
  manifest.target_id = static_cast<uint32_t>(header.target);
  manifest.validity = header.validity;
  manifest.hash = hash;
  return manifest;
}

bool write_membership(Writer& writer,
                      const MembershipFunction& value) noexcept {
  if (!writer.id(value.id) ||
      !writer.u8(static_cast<uint8_t>(value.shape))) return false;
  for (const float point : value.points) {
    if (!writer.f32(point)) return false;
  }
  return true;
}

bool read_membership(Reader& reader, MembershipFunction& value) noexcept {
  uint8_t shape{0U};
  if (!reader.id(value.id) || !reader.u8(shape)) return false;
  value.shape = static_cast<MembershipShape>(shape);
  for (float& point : value.points) {
    if (!reader.f32(point)) return false;
  }
  return true;
}

template <typename Definition>
bool write_definition(Writer& writer, const Definition& value,
                      bool has_input_id) noexcept {
  if (has_input_id &&
      !writer.u8(static_cast<uint8_t>(value.id))) return false;
  if (!writer.u8(static_cast<uint8_t>(value.unit)) ||
      !writer.f32(value.domain_min) || !writer.f32(value.domain_max) ||
      !writer.u8(value.set_count)) return false;
  for (std::size_t index = 0U; index < value.set_count; ++index) {
    if (!write_membership(writer, value.sets[index])) return false;
  }
  return true;
}

bool write_output(Writer& writer, const FisOutputDefinition& value) noexcept {
  if (!writer.id(value.id) ||
      !writer.u8(static_cast<uint8_t>(value.unit)) ||
      !writer.f32(value.domain_min) || !writer.f32(value.domain_max) ||
      !writer.u8(value.set_count)) return false;
  for (std::size_t index = 0U; index < value.set_count; ++index) {
    if (!write_membership(writer, value.sets[index])) return false;
  }
  return true;
}

bool read_input(Reader& reader, FisInputDefinition& value) noexcept {
  uint8_t input{0U};
  uint8_t unit{0U};
  if (!reader.u8(input) || !reader.u8(unit) ||
      !reader.f32(value.domain_min) || !reader.f32(value.domain_max) ||
      !reader.u8(value.set_count) || value.set_count > value.sets.size()) {
    return false;
  }
  value.id = static_cast<ModelInput>(input);
  value.unit = static_cast<ModelUnit>(unit);
  for (std::size_t index = 0U; index < value.set_count; ++index) {
    if (!read_membership(reader, value.sets[index])) return false;
  }
  return true;
}

bool read_output(Reader& reader, FisOutputDefinition& value) noexcept {
  uint8_t unit{0U};
  if (!reader.id(value.id) || !reader.u8(unit) ||
      !reader.f32(value.domain_min) || !reader.f32(value.domain_max) ||
      !reader.u8(value.set_count) || value.set_count > value.sets.size()) {
    return false;
  }
  value.unit = static_cast<ModelUnit>(unit);
  for (std::size_t index = 0U; index < value.set_count; ++index) {
    if (!read_membership(reader, value.sets[index])) return false;
  }
  return true;
}

}  // namespace

config::ConfigHash EncodedModelProfile::hash() const noexcept {
  Header header{};
  if (!parse_header(*this, header)) return {};
  config::ConfigHash output{};
  std::copy_n(bytes.begin() + kModelEnvelopeHeaderBytes + header.payload_size,
              output.size(), output.begin());
  return output;
}

bool ModelProfileCodec::encode(const FisModelProfile& profile,
                               EncodedModelProfile& output) const noexcept {
  if (!validate_fis_model(profile).valid()) return false;
  Writer writer(output);
  if (!write_header(writer, profile.manifest, profile.deployment) ||
      !writer.provenance(profile.manifest.provenance) ||
      !writer.u8(profile.input_count)) return false;
  for (std::size_t index = 0U; index < profile.input_count; ++index) {
    if (!write_definition(writer, profile.inputs[index], true)) return false;
  }
  if (!write_output(writer, profile.output) ||
      !writer.u8(profile.rule_count)) return false;
  for (std::size_t index = 0U; index < profile.rule_count; ++index) {
    const auto& rule = profile.rules[index];
    if (!writer.u16(rule.id) || !writer.u8(rule.antecedent_count)) return false;
    for (std::size_t input = 0U; input < rule.antecedent_count; ++input) {
      if (!writer.u8(rule.antecedent_set_indices[input])) return false;
    }
    if (!writer.u8(static_cast<uint8_t>(rule.antecedent_operator)) ||
        !writer.u8(rule.consequent_set_index)) return false;
  }
  if (!writer.u8(profile.label_count)) return false;
  for (std::size_t index = 0U; index < profile.label_count; ++index) {
    if (!writer.id(profile.labels[index].id) ||
        !writer.f32(profile.labels[index].minimum_inclusive)) return false;
  }
  if (!writer.u8(static_cast<uint8_t>(profile.clipping)) ||
      !writer.u8(static_cast<uint8_t>(profile.aggregation)) ||
      !writer.u16(profile.centroid_grid_points) ||
      !writer.f32(profile.area_epsilon)) return false;
  if (writer.offset() - kModelEnvelopeHeaderBytes > kMaxModelPayloadBytes) {
    output = EncodedModelProfile{};
    return false;
  }
  finish(output, writer.offset());
  return true;
}

bool ModelProfileCodec::encode(const SafetyModelProfile& profile,
                               EncodedModelProfile& output) const noexcept {
  if (!validate_safety_model(profile).valid()) return false;
  Writer writer(output);
  if (!write_header(writer, profile.manifest, profile.deployment) ||
      !writer.provenance(profile.manifest.provenance) ||
      !writer.u8(profile.channel_count)) return false;
  for (std::size_t index = 0U; index < profile.channel_count; ++index) {
    const auto& channel = profile.channels[index];
    if (!writer.u8(static_cast<uint8_t>(channel.signal)) ||
        !writer.u8(channel.required ? 1U : 0U) ||
        !writer.f32(channel.alert_at_or_above) ||
        !writer.f32(channel.clear_below)) return false;
  }
  if (!writer.u16(profile.clear_consecutive_cycles)) return false;
  finish(output, writer.offset());
  return true;
}

bool ModelProfileCodec::decode(const EncodedModelProfile& encoded,
                               FisModelProfile& output) const noexcept {
  Header header{};
  if (!parse_header(encoded, header) || !hash_matches(encoded, header) ||
      header.kind != acquisition::ProfileKind::Fis) return false;
  FisModelProfile candidate{};
  candidate.manifest = manifest_from(header, encoded.hash());
  candidate.deployment = header.deployment;
  Reader reader(encoded.bytes.data() + kModelEnvelopeHeaderBytes,
                header.payload_size);
  if (!reader.provenance(candidate.manifest.provenance) ||
      !reader.u8(candidate.input_count) ||
      candidate.input_count > candidate.inputs.size()) return false;
  for (std::size_t index = 0U; index < candidate.input_count; ++index) {
    if (!read_input(reader, candidate.inputs[index])) return false;
  }
  if (!read_output(reader, candidate.output) ||
      !reader.u8(candidate.rule_count) ||
      candidate.rule_count > candidate.rules.size()) return false;
  for (std::size_t index = 0U; index < candidate.rule_count; ++index) {
    auto& rule = candidate.rules[index];
    uint8_t operation{0U};
    if (!reader.u16(rule.id) || !reader.u8(rule.antecedent_count) ||
        rule.antecedent_count > rule.antecedent_set_indices.size()) return false;
    for (std::size_t input = 0U; input < rule.antecedent_count; ++input) {
      if (!reader.u8(rule.antecedent_set_indices[input])) return false;
    }
    if (!reader.u8(operation) || !reader.u8(rule.consequent_set_index)) return false;
    rule.antecedent_operator = static_cast<AntecedentOperator>(operation);
  }
  if (!reader.u8(candidate.label_count) ||
      candidate.label_count > candidate.labels.size()) return false;
  for (std::size_t index = 0U; index < candidate.label_count; ++index) {
    if (!reader.id(candidate.labels[index].id) ||
        !reader.f32(candidate.labels[index].minimum_inclusive)) return false;
  }
  uint8_t clipping{0U};
  uint8_t aggregation{0U};
  if (!reader.u8(clipping) || !reader.u8(aggregation) ||
      !reader.u16(candidate.centroid_grid_points) ||
      !reader.f32(candidate.area_epsilon) || reader.remaining() != 0U) return false;
  candidate.clipping = static_cast<ClippingOperator>(clipping);
  candidate.aggregation = static_cast<AggregationOperator>(aggregation);
  if (!validate_fis_model(candidate).valid()) return false;
  output = candidate;
  return true;
}

bool ModelProfileCodec::decode(const EncodedModelProfile& encoded,
                               SafetyModelProfile& output) const noexcept {
  Header header{};
  if (!parse_header(encoded, header) || !hash_matches(encoded, header) ||
      header.kind != acquisition::ProfileKind::Safety) return false;
  SafetyModelProfile candidate{};
  candidate.manifest = manifest_from(header, encoded.hash());
  candidate.deployment = header.deployment;
  Reader reader(encoded.bytes.data() + kModelEnvelopeHeaderBytes,
                header.payload_size);
  if (!reader.provenance(candidate.manifest.provenance) ||
      !reader.u8(candidate.channel_count) ||
      candidate.channel_count > candidate.channels.size()) return false;
  for (std::size_t index = 0U; index < candidate.channel_count; ++index) {
    auto& channel = candidate.channels[index];
    uint8_t signal{0U};
    uint8_t required{0U};
    if (!reader.u8(signal) || !reader.u8(required) || required > 1U ||
        !reader.f32(channel.alert_at_or_above) ||
        !reader.f32(channel.clear_below)) return false;
    channel.signal = static_cast<HazardSignal>(signal);
    channel.required = required != 0U;
  }
  if (!reader.u16(candidate.clear_consecutive_cycles) ||
      reader.remaining() != 0U || !validate_safety_model(candidate).valid()) {
    return false;
  }
  output = candidate;
  return true;
}

bool ModelProfileCodec::identity(const EncodedModelProfile& encoded,
                                 ModelProfileIdentity& output) const noexcept {
  Header header{};
  if (!parse_header(encoded, header) || !hash_matches(encoded, header)) {
    return false;
  }
  output = ModelProfileIdentity{
      ModelProfileKey{header.kind, header.target}, header.schema,
      header.version, header.validity, header.deployment, encoded.hash()};
  return true;
}

bool ModelProfileCodec::validate(const EncodedModelProfile& encoded,
                                 ModelProfileKey expected) const noexcept {
  ModelProfileIdentity actual{};
  if (!valid_model_key(expected) || !identity(encoded, actual) ||
      !(actual.key == expected)) return false;
  if (expected.kind == acquisition::ProfileKind::Fis) {
    FisModelProfile decoded{};
    return decode(encoded, decoded);
  }
  SafetyModelProfile decoded{};
  return decode(encoded, decoded);
}

}  // namespace sigap::models
