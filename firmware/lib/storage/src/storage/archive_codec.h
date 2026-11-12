#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "acquisition/acquisition_types.h"
#include "telemetry/sha256.h"

namespace sigap::storage {

inline constexpr uint32_t kArchiveMagic{0x53474152U};
inline constexpr uint16_t kArchiveFormatVersion{1U};
inline constexpr std::size_t kArchiveHeaderBytes{60U};
inline constexpr std::size_t kArchiveTrailerBytes{4U};
inline constexpr std::size_t kArchiveSampleExtensionBytes{9U};
inline constexpr std::size_t kArchiveMaxRecordBytes{16U * 1024U};
inline constexpr uint32_t kArchiveCommitMarker{0xC0A117EDU};
inline constexpr std::size_t kArchiveMagicOffset{0U};
inline constexpr std::size_t kArchiveVersionOffset{4U};
inline constexpr std::size_t kArchiveKindOffset{6U};
inline constexpr std::size_t kArchivePayloadSizeOffset{8U};
inline constexpr std::size_t kArchiveSequenceOffset{12U};
inline constexpr std::size_t kArchiveDeviceIdOffset{20U};
inline constexpr std::size_t kArchiveBootIdOffset{36U};
inline constexpr std::size_t kArchivePayloadCrcOffset{52U};
inline constexpr std::size_t kArchiveHeaderCrcOffset{56U};

enum class ArchiveRecordKind : uint8_t { Sample = 1U, Result = 2U, Event = 3U, Metadata = 4U };

struct ArchiveIdentity final {
  acquisition::DeviceId device_id{};
  acquisition::BootId boot_id{};
  uint64_t sequence{0U};
  ArchiveRecordKind kind{ArchiveRecordKind::Sample};
};

struct ImmutableRecordBytes final {
  std::array<uint8_t, kArchiveMaxRecordBytes> bytes{};
  std::size_t size{0U};
  ArchiveIdentity identity{};
  std::array<uint8_t, 32U> hash{};
  bool committed{false};
};

struct ArchiveRecordView final {
  ArchiveIdentity identity{};
  const uint8_t* payload{nullptr};
  std::size_t payload_size{0U};
};

// A typed view keeps the decoder bounded while allowing the batch builder to
// reject a record kind before interpreting its payload. The payload remains
// caller-owned immutable bytes; kind-specific decoders can consume it with
// PayloadReader without copying a 16 KiB frame.
using TypedArchiveRecordView = ArchiveRecordView;

inline constexpr std::size_t kArchiveMaxProfileRefs{16U};
inline constexpr std::size_t kArchiveMaxSourceIds{128U};
inline constexpr std::size_t kArchiveMaxFisMemberships{32U};
inline constexpr std::size_t kArchiveMaxFisRules{32U};
inline constexpr std::size_t kArchiveMaxSafetyEvidence{8U};
inline constexpr std::size_t kArchiveModelIdBytes{16U};

enum class TypedDecodeStatus : uint8_t {
  Decoded,
  InvalidFrame,
  WrongKind,
  MalformedPayload,
  BoundsExceeded,
  UnsupportedKind,
};

struct DecodedConfigRef final {
  uint32_t schema_version{0U};
  uint64_t config_version{0U};
  config::ConfigHash hash{};
  bool present{false};
};

struct DecodedProfileRef final {
  uint8_t kind{0U};
  uint32_t schema_version{0U};
  uint32_t profile_version{0U};
  uint32_t target_id{0U};
  config::ConfigHash hash{};
  bool present{false};
};

struct DecodedSourceId final {
  acquisition::DeviceId device_id{};
  acquisition::BootId boot_id{};
  uint64_t sequence{0U};
};

struct DecodedMetric final {
  bool available{false};
  float value{0.0F};
  uint8_t quality{0U};
  uint8_t reason{0U};
};

struct DecodedFisMembership final {
  uint8_t input{0U};
  std::array<char, kArchiveModelIdBytes> set_id{};
  float value{0.0F};
};

struct DecodedFisRule final {
  uint16_t id{0U};
  float strength{0.0F};
  std::array<char, kArchiveModelIdBytes> output_id{};
};

struct DecodedFisTrace final {
  uint32_t model_version{0U};
  config::ConfigHash model_hash{};
  std::array<DecodedFisMembership, kArchiveMaxFisMemberships> memberships{};
  std::array<DecodedFisRule, kArchiveMaxFisRules> rules{};
  std::array<char, kArchiveModelIdBytes> label{};
  std::array<char, kArchiveModelIdBytes> aggregate_output_id{};
  uint16_t membership_count{0U};
  uint16_t rule_count{0U};
  uint8_t state{0U};
  uint8_t reason{0U};
  uint8_t model_state{0U};
  bool score_present{false};
  float score{0.0F};
  bool label_present{false};
  bool trace_present{false};
  bool aggregate_present{false};
  float aggregate_area{0.0F};
  bool centroid_present{false};
  float aggregate_centroid{0.0F};
};

struct DecodedSafetyEvidence final {
  uint8_t signal{0U};
  uint8_t input_state{0U};
  float value{0.0F};
  float alert_at_or_above{0.0F};
  float clear_below{0.0F};
  bool threshold_exceeded{false};
  bool clear_condition_met{false};
};

struct DecodedSafetyTrace final {
  DecodedProfileRef model_ref{};
  std::array<DecodedSafetyEvidence, kArchiveMaxSafetyEvidence> evidence{};
  uint8_t evidence_count{0U};
  uint8_t clear_streak{0U};
  bool alert_latched_before{false};
  bool alert_latched_after{false};
};

struct DecodedResultPayload final {
  uint8_t stream{0U};
  uint64_t computed_monotonic_us{0U};
  bool recorded_at_available{false};
  int64_t recorded_at_epoch_ms{0};
  uint8_t time_quality{0U};
  DecodedConfigRef config_ref{};
  std::array<DecodedProfileRef, kArchiveMaxProfileRefs> profile_refs{};
  std::array<DecodedSourceId, kArchiveMaxSourceIds> source_sample_ids{};
  std::array<DecodedMetric, 8U> metrics{};
  uint16_t profile_ref_count{0U};
  uint16_t source_sample_id_count{0U};
  DecodedFisTrace fis{};
  uint8_t safety_state{0U};
  uint8_t safety_reason{0U};
  uint8_t safety_model_state{0U};
  DecodedSafetyTrace safety_trace{};
};

struct DecodedEventPayload final {
  uint8_t event_kind{0U};
  uint64_t monotonic_us{0U};
  DecodedConfigRef config_ref{};
  bool pending_activation{false};
  DecodedProfileRef previous{};
  DecodedProfileRef current{};
};

enum class ArchiveMetadataType : uint8_t {
  ConfigActivation = 1U,
  ProfileActivation = 2U,
  Boot = 3U,
  HealthSnapshot = 4U,
};

struct DecodedMetadataPayload final {
  ArchiveMetadataType type{ArchiveMetadataType::Boot};
  uint64_t monotonic_us{0U};
  bool recorded_at_available{false};
  int64_t recorded_at_epoch_ms{0};
  uint8_t time_quality{0U};
  DecodedConfigRef config_ref{};
  std::array<DecodedProfileRef, kArchiveMaxProfileRefs> profile_refs{};
  uint16_t profile_ref_count{0U};
  std::array<char, 64U> firmware_version{};
  uint8_t reset_reason{0U};
};

class PayloadWriter final {
 public:
  PayloadWriter(uint8_t* data, std::size_t capacity) noexcept
      : data_(data), capacity_(capacity) {}

  bool u8(uint8_t value) noexcept { return put(&value, sizeof(value)); }
  bool u16(uint16_t value) noexcept { return integer(value); }
  bool u32(uint32_t value) noexcept { return integer(value); }
  bool u64(uint64_t value) noexcept { return integer(value); }
  bool i32(int32_t value) noexcept { return u32(static_cast<uint32_t>(value)); }
  bool i64(int64_t value) noexcept { return u64(static_cast<uint64_t>(value)); }
  bool f32(float value) noexcept {
    uint32_t bits{0U};
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return u32(bits);
  }
  bool bytes(const uint8_t* value, std::size_t size) noexcept {
    return value != nullptr && put(value, size);
  }
  std::size_t size() const noexcept { return offset_; }

 private:
  template <typename T>
  bool integer(T value) noexcept {
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
      const uint8_t byte = static_cast<uint8_t>(value >> (i * 8U));
      if (!put(&byte, 1U)) return false;
    }
    return true;
  }
  bool put(const uint8_t* value, std::size_t size) noexcept {
    if (value == nullptr || offset_ > capacity_ || size > capacity_ - offset_) {
      return false;
    }
    std::memcpy(data_ + offset_, value, size);
    offset_ += size;
    return true;
  }
  uint8_t* data_{nullptr};
  std::size_t capacity_{0U};
  std::size_t offset_{0U};
};

class PayloadReader final {
 public:
  PayloadReader(const uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  bool u8(uint8_t& value) noexcept { return get(&value, sizeof(value)); }
  bool u16(uint16_t& value) noexcept { return integer(value); }
  bool u32(uint32_t& value) noexcept { return integer(value); }
  bool u64(uint64_t& value) noexcept { return integer(value); }
  bool i32(int32_t& value) noexcept {
    uint32_t raw{0U};
    if (!u32(raw)) return false;
    value = static_cast<int32_t>(raw);
    return true;
  }
  bool i64(int64_t& value) noexcept {
    uint64_t raw{0U};
    if (!u64(raw)) return false;
    value = static_cast<int64_t>(raw);
    return true;
  }
  bool f32(float& value) noexcept {
    uint32_t bits{0U};
    if (!u32(bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }
  bool bytes(uint8_t* value, std::size_t size) noexcept {
    return value != nullptr && get(value, size);
  }
  bool finished() const noexcept { return offset_ == size_; }

 private:
  template <typename T>
  bool integer(T& value) noexcept {
    if (offset_ > size_ || sizeof(T) > size_ - offset_) return false;
    value = 0U;
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
      value |= static_cast<T>(data_[offset_ + i]) << (i * 8U);
    }
    offset_ += sizeof(T);
    return true;
  }
  bool get(uint8_t* value, std::size_t size) noexcept {
    if (data_ == nullptr || value == nullptr ||
        offset_ > size_ || size > size_ - offset_) {
      return false;
    }
    std::memcpy(value, data_ + offset_, size);
    offset_ += size;
    return true;
  }
  const uint8_t* data_{nullptr};
  std::size_t size_{0U};
  std::size_t offset_{0U};
};

inline uint32_t archive_crc32(const uint8_t* data, std::size_t size) noexcept {
  uint32_t crc{0xFFFFFFFFU};
  for (std::size_t i = 0U; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
  }
  return ~crc;
}

class RecordCodec final {
 public:
  static bool begin(const ArchiveIdentity& identity,
                    ImmutableRecordBytes& output,
                    PayloadWriter& writer) noexcept {
    if (!identity.device_id.present || !identity.boot_id.present) return false;
    output.bytes.fill(0U);
    output.size = 0U;
    output.identity = identity;
    output.hash.fill(0U);
    output.committed = false;
    writer = PayloadWriter(output.bytes.data() + kArchiveHeaderBytes,
                           output.bytes.size() - kArchiveHeaderBytes - kArchiveTrailerBytes);
    return true;
  }

  static bool finish(ImmutableRecordBytes& output,
                     const PayloadWriter& writer) noexcept {
    const std::size_t payload_size = writer.size();
    const std::size_t total = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
    if (total > output.bytes.size()) return false;
    put_u32(output.bytes.data() + kArchiveMagicOffset, kArchiveMagic);
    put_u16(output.bytes.data() + kArchiveVersionOffset, kArchiveFormatVersion);
    output.bytes[kArchiveKindOffset] = static_cast<uint8_t>(output.identity.kind);
    output.bytes[7U] = 0U;
    put_u32(output.bytes.data() + kArchivePayloadSizeOffset,
            static_cast<uint32_t>(payload_size));
    put_u64(output.bytes.data() + kArchiveSequenceOffset, output.identity.sequence);
    std::memcpy(output.bytes.data() + kArchiveDeviceIdOffset,
                output.identity.device_id.bytes.data(),
                output.identity.device_id.bytes.size());
    std::memcpy(output.bytes.data() + kArchiveBootIdOffset,
                output.identity.boot_id.bytes.data(),
                output.identity.boot_id.bytes.size());
    put_u32(output.bytes.data() + kArchivePayloadCrcOffset,
            archive_crc32(output.bytes.data() + kArchiveHeaderBytes, payload_size));
    put_u32(output.bytes.data() + kArchiveHeaderCrcOffset,
            archive_crc32(output.bytes.data(), kArchiveHeaderCrcOffset));
    put_u32(output.bytes.data() + kArchiveHeaderBytes + payload_size,
            kArchiveCommitMarker);
    output.size = total;
    output.hash = telemetry_contract::sha256(std::string_view(
        reinterpret_cast<const char*>(output.bytes.data()), output.size));
    output.committed = true;
    return true;
  }

  static bool valid(const ImmutableRecordBytes& record) noexcept {
    return record.committed &&
           valid_frame(record.bytes.data(), record.size);
  }

  static bool valid_frame(const uint8_t* bytes, std::size_t size) noexcept {
    if (bytes == nullptr || size < kArchiveHeaderBytes + kArchiveTrailerBytes ||
        size > kArchiveMaxRecordBytes) return false;
    if (get_u32(bytes + kArchiveMagicOffset) != kArchiveMagic ||
        get_u16(bytes + kArchiveVersionOffset) != kArchiveFormatVersion ||
        bytes[kArchiveKindOffset] < static_cast<uint8_t>(ArchiveRecordKind::Sample) ||
        bytes[kArchiveKindOffset] > static_cast<uint8_t>(ArchiveRecordKind::Metadata)) {
      return false;
    }
    const std::size_t payload_size = get_u32(bytes + kArchivePayloadSizeOffset);
    if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                           kArchiveTrailerBytes ||
        size != kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes) {
      return false;
    }
    if (get_u32(bytes + kArchiveHeaderCrcOffset) !=
        archive_crc32(bytes, kArchiveHeaderCrcOffset)) return false;
    if (get_u32(bytes + kArchivePayloadCrcOffset) !=
        archive_crc32(bytes + kArchiveHeaderBytes, payload_size)) return false;
    return get_u32(bytes + kArchiveHeaderBytes + payload_size) ==
           kArchiveCommitMarker;
  }

  static bool read(const ImmutableRecordBytes& record,
                   ArchiveRecordView& view) noexcept {
    view = ArchiveRecordView{};
    if (!valid(record)) return false;
    view.identity.kind = static_cast<ArchiveRecordKind>(
        record.bytes[kArchiveKindOffset]);
    view.identity.sequence = get_u64(record.bytes.data() + kArchiveSequenceOffset);
    view.identity.device_id.present = true;
    view.identity.boot_id.present = true;
    std::memcpy(view.identity.device_id.bytes.data(),
                record.bytes.data() + kArchiveDeviceIdOffset,
                view.identity.device_id.bytes.size());
    std::memcpy(view.identity.boot_id.bytes.data(),
                record.bytes.data() + kArchiveBootIdOffset,
                view.identity.boot_id.bytes.size());
    view.payload = record.bytes.data() + kArchiveHeaderBytes;
    view.payload_size = get_u32(record.bytes.data() + kArchivePayloadSizeOffset);
    return true;
  }

  static bool decode_result_view(const ImmutableRecordBytes& record,
                                 TypedArchiveRecordView& view) noexcept {
    return read(record, view) && view.identity.kind == ArchiveRecordKind::Result;
  }

  static bool decode_event_view(const ImmutableRecordBytes& record,
                                TypedArchiveRecordView& view) noexcept {
    return read(record, view) && view.identity.kind == ArchiveRecordKind::Event;
  }

  static bool decode_metadata_view(const ImmutableRecordBytes& record,
                                   TypedArchiveRecordView& view) noexcept {
    return read(record, view) && view.identity.kind == ArchiveRecordKind::Metadata;
  }

  // Result and event payloads are decoded into bounded value objects. Metadata
  // has no producer-defined wire schema yet, so it is explicitly rejected
  // instead of being treated as delivered merely because its frame is valid.
  static TypedDecodeStatus decode_result(const ImmutableRecordBytes& record,
                                         DecodedResultPayload& output) noexcept {
    output = DecodedResultPayload{};
    ArchiveRecordView view{};
    if (!read(record, view)) return TypedDecodeStatus::InvalidFrame;
    if (view.identity.kind != ArchiveRecordKind::Result) {
      return TypedDecodeStatus::WrongKind;
    }
    PayloadReader reader{view.payload, view.payload_size};
    uint8_t flag{0U};
    if (!reader.u8(output.stream) || !reader.u64(output.computed_monotonic_us) ||
        !reader.u8(flag) || !reader.i64(output.recorded_at_epoch_ms) ||
        !reader.u8(output.time_quality) || !read_config_ref(reader, output.config_ref)) {
      return TypedDecodeStatus::MalformedPayload;
    }
    output.recorded_at_available = flag != 0U;
    uint16_t count{0U};
    if (!reader.u16(count)) return TypedDecodeStatus::MalformedPayload;
    if (count > kArchiveMaxProfileRefs) return TypedDecodeStatus::BoundsExceeded;
    output.profile_ref_count = count;
    for (uint16_t index = 0U; index < count; ++index) {
      if (!read_profile_ref(reader, output.profile_refs[index])) {
        return TypedDecodeStatus::MalformedPayload;
      }
    }
    if (!reader.u16(count)) return TypedDecodeStatus::MalformedPayload;
    if (count > kArchiveMaxSourceIds) return TypedDecodeStatus::BoundsExceeded;
    output.source_sample_id_count = count;
    for (uint16_t index = 0U; index < count; ++index) {
      auto& source = output.source_sample_ids[index];
      if (!reader.bytes(source.device_id.bytes.data(), source.device_id.bytes.size()) ||
          !reader.bytes(source.boot_id.bytes.data(), source.boot_id.bytes.size()) ||
          !reader.u64(source.sequence)) {
        return TypedDecodeStatus::MalformedPayload;
      }
      source.device_id.present = true;
      source.boot_id.present = true;
    }
    for (auto& metric : output.metrics) {
      if (!reader.u8(flag) || !reader.f32(metric.value) ||
          !reader.u8(metric.quality) || !reader.u8(metric.reason)) {
        return TypedDecodeStatus::MalformedPayload;
      }
      metric.available = flag != 0U;
    }
    auto& fis = output.fis;
    uint8_t score_flag{0U};
    uint8_t label_flag{0U};
    uint8_t trace_present_flag{0U};
    if (!reader.u8(fis.state) || !reader.u8(fis.reason) ||
        !reader.u8(fis.model_state) || !reader.u8(score_flag) ||
        !reader.f32(fis.score) || !reader.u8(label_flag)) {
      return TypedDecodeStatus::MalformedPayload;
    }
    fis.score_present = score_flag != 0U;
    if (!reader.bytes(reinterpret_cast<uint8_t*>(fis.label.data()),
                      fis.label.size()) ||
        !reader.u8(trace_present_flag) ||
        !reader.u32(fis.model_version) ||
        !reader.bytes(fis.model_hash.data(), fis.model_hash.size()) ||
        !reader.u16(fis.membership_count)) {
      return TypedDecodeStatus::MalformedPayload;
    }
    fis.label_present = label_flag != 0U;
    fis.trace_present = trace_present_flag != 0U;
    if (fis.membership_count > kArchiveMaxFisMemberships) {
      return TypedDecodeStatus::BoundsExceeded;
    }
    for (uint16_t index = 0U; index < fis.membership_count; ++index) {
      auto& membership = fis.memberships[index];
      if (!reader.u8(membership.input) ||
          !reader.bytes(reinterpret_cast<uint8_t*>(membership.set_id.data()),
                        membership.set_id.size()) ||
          !reader.f32(membership.value)) {
        return TypedDecodeStatus::MalformedPayload;
      }
    }
    if (!reader.u16(fis.rule_count)) return TypedDecodeStatus::MalformedPayload;
    if (fis.rule_count > kArchiveMaxFisRules) return TypedDecodeStatus::BoundsExceeded;
    for (uint16_t index = 0U; index < fis.rule_count; ++index) {
      auto& rule = fis.rules[index];
      if (!reader.u16(rule.id) || !reader.f32(rule.strength) ||
          !reader.bytes(reinterpret_cast<uint8_t*>(rule.output_id.data()),
                        rule.output_id.size())) {
        return TypedDecodeStatus::MalformedPayload;
      }
    }
    uint8_t aggregate_flag{0U};
    uint8_t centroid_flag{0U};
    if (!reader.u8(aggregate_flag) ||
        !reader.bytes(reinterpret_cast<uint8_t*>(fis.aggregate_output_id.data()),
                      fis.aggregate_output_id.size()) ||
        !reader.f32(fis.aggregate_area) || !reader.u8(centroid_flag) ||
        !reader.f32(fis.aggregate_centroid)) {
      return TypedDecodeStatus::MalformedPayload;
    }
    fis.aggregate_present = aggregate_flag != 0U;
    fis.centroid_present = centroid_flag != 0U;
    if (!reader.u8(output.safety_state) || !reader.u8(output.safety_reason) ||
        !reader.u8(output.safety_model_state) ||
        !read_profile_ref(reader, output.safety_trace.model_ref) ||
        !reader.u8(output.safety_trace.evidence_count) ||
        !reader.u8(output.safety_trace.clear_streak) ||
        !reader.u8(flag)) {
      return TypedDecodeStatus::MalformedPayload;
    }
    output.safety_trace.alert_latched_before = flag != 0U;
    if (!reader.u8(flag)) return TypedDecodeStatus::MalformedPayload;
    output.safety_trace.alert_latched_after = flag != 0U;
    if (output.safety_trace.evidence_count > kArchiveMaxSafetyEvidence) {
      return TypedDecodeStatus::BoundsExceeded;
    }
    for (uint8_t index = 0U; index < output.safety_trace.evidence_count; ++index) {
      auto& evidence = output.safety_trace.evidence[index];
      if (!reader.u8(evidence.signal) || !reader.u8(evidence.input_state) ||
          !reader.f32(evidence.value) || !reader.f32(evidence.alert_at_or_above) ||
          !reader.f32(evidence.clear_below) || !reader.u8(flag)) {
        return TypedDecodeStatus::MalformedPayload;
      }
      evidence.threshold_exceeded = flag != 0U;
      if (!reader.u8(flag)) return TypedDecodeStatus::MalformedPayload;
      evidence.clear_condition_met = flag != 0U;
    }
    return reader.finished() ? TypedDecodeStatus::Decoded
                             : TypedDecodeStatus::MalformedPayload;
  }

  static TypedDecodeStatus decode_event(const ImmutableRecordBytes& record,
                                        DecodedEventPayload& output) noexcept {
    output = DecodedEventPayload{};
    ArchiveRecordView view{};
    if (!read(record, view)) return TypedDecodeStatus::InvalidFrame;
    if (view.identity.kind != ArchiveRecordKind::Event) {
      return TypedDecodeStatus::WrongKind;
    }
    PayloadReader reader{view.payload, view.payload_size};
    uint8_t config_flag{0U};
    uint8_t pending_flag{0U};
    if (!reader.u8(output.event_kind) || !reader.u64(output.monotonic_us) ||
        !reader.u8(config_flag) || !reader.u32(output.config_ref.schema_version) ||
        !reader.u64(output.config_ref.config_version) ||
        !reader.bytes(output.config_ref.hash.data(), output.config_ref.hash.size()) ||
        !reader.u8(pending_flag) || !read_profile_ref(reader, output.previous) ||
        !read_profile_ref(reader, output.current)) {
      return TypedDecodeStatus::MalformedPayload;
    }
    output.config_ref.present = config_flag != 0U;
    output.pending_activation = pending_flag != 0U;
    return reader.finished() ? TypedDecodeStatus::Decoded
                             : TypedDecodeStatus::MalformedPayload;
  }

  static TypedDecodeStatus decode_metadata(
      const ImmutableRecordBytes& record,
      DecodedMetadataPayload& output) noexcept {
    output = DecodedMetadataPayload{};
    ArchiveRecordView view{};
    if (!read(record, view)) return TypedDecodeStatus::InvalidFrame;
    if (view.identity.kind != ArchiveRecordKind::Metadata) {
      return TypedDecodeStatus::WrongKind;
    }
    PayloadReader reader{view.payload, view.payload_size};
    uint8_t type{0U};
    uint8_t flag{0U};
    if (!reader.u8(type) || type < static_cast<uint8_t>(ArchiveMetadataType::ConfigActivation) ||
        type > static_cast<uint8_t>(ArchiveMetadataType::HealthSnapshot) ||
        !reader.u64(output.monotonic_us) || !reader.u8(flag) ||
        !reader.i64(output.recorded_at_epoch_ms) || !reader.u8(output.time_quality) ||
        !read_config_ref(reader, output.config_ref)) {
      return TypedDecodeStatus::MalformedPayload;
    }
    output.type = static_cast<ArchiveMetadataType>(type);
    output.recorded_at_available = flag != 0U;
    uint16_t count{0U};
    if (!reader.u16(count) || count > kArchiveMaxProfileRefs) {
      return TypedDecodeStatus::BoundsExceeded;
    }
    output.profile_ref_count = count;
    for (uint16_t index = 0U; index < count; ++index) {
      if (!read_profile_ref(reader, output.profile_refs[index])) {
        return TypedDecodeStatus::MalformedPayload;
      }
    }
    if (output.type == ArchiveMetadataType::Boot) {
      uint8_t length{0U};
      if (!reader.u8(length) || length == 0U || length >= output.firmware_version.size() ||
          !reader.bytes(reinterpret_cast<uint8_t*>(output.firmware_version.data()), length) ||
          !reader.u8(output.reset_reason)) {
        return TypedDecodeStatus::MalformedPayload;
      }
      output.firmware_version[length] = '\0';
    }
    return reader.finished() ? TypedDecodeStatus::Decoded
                             : TypedDecodeStatus::MalformedPayload;
  }

  // Compatibility view for callers that only need kind validation. Typed
  // metadata consumers must use the overload above so payload semantics are
  // checked before batching.
  static TypedDecodeStatus decode_metadata(
      const ImmutableRecordBytes& record,
      TypedArchiveRecordView& view) noexcept {
    return decode_metadata_view(record, view)
               ? TypedDecodeStatus::UnsupportedKind
               : TypedDecodeStatus::InvalidFrame;
  }

  static bool encode_sample(const acquisition::SampleRecord& sample,
                            ImmutableRecordBytes& output) noexcept {
    const ArchiveIdentity identity{sample.header.device_id, sample.header.boot_id,
                                   sample.header.sequence, ArchiveRecordKind::Sample};
    PayloadWriter writer{nullptr, 0U};
    if (!begin(identity, output, writer)) return false;
    writer = PayloadWriter(output.bytes.data() + kArchiveHeaderBytes,
                           output.bytes.size() - kArchiveHeaderBytes - kArchiveTrailerBytes);
    if (!writer.u8(static_cast<uint8_t>(sample.header.sensor)) ||
        !writer.u8(static_cast<uint8_t>(sample.header.stream)) ||
        !writer.u8(static_cast<uint8_t>(sample.header.kind)) ||
        !writer.u64(sample.header.monotonic_us) ||
        !writer.u8(static_cast<uint8_t>(sample.quality)) ||
        !writer.u8(static_cast<uint8_t>(sample.reason)) ||
        !writer.u8(sample.header.recorded_at.available ? 1U : 0U) ||
        !writer.i64(sample.header.recorded_at.epoch_ms) ||
        !writer.u8(static_cast<uint8_t>(sample.header.time_quality)) ||
        !writer.u32(sample.header.config_ref.schema_version) ||
        !writer.u64(sample.header.config_ref.config_version) ||
        !writer.u8(sample.header.config_ref.present ? 1U : 0U) ||
        !writer.bytes(sample.header.config_ref.hash.data(), sample.header.config_ref.hash.size()) ||
        !writer.u8(sample.raw.present ? 1U : 0U) ||
        !writer.u8(sample.raw.crc_valid ? 1U : 0U) ||
        !writer.u8(sample.raw.conversion_complete ? 1U : 0U) ||
        !writer.u8(sample.raw.identity_valid ? 1U : 0U) ||
        !writer.u8(sample.raw.temperature_crc_valid ? 1U : 0U) ||
        !writer.u8(sample.raw.humidity_crc_valid ? 1U : 0U) ||
        !writer.u8(sample.raw.heater_ready ? 1U : 0U) ||
        !writer.i64(sample.raw.signed_counts) || !writer.u16(sample.raw.adc_counts) ||
        !writer.u32(sample.raw.distance_us) || !writer.i32(sample.raw.temperature_centi_c) ||
        !writer.i32(sample.raw.humidity_centi_pct) ||
        !writer.u8(sample.raw.ds_wire_evidence ? 1U : 0U) ||
        !writer.bytes(sample.raw.ds_rom.data(), sample.raw.ds_rom.size()) ||
        !writer.bytes(sample.raw.ds_scratchpad.data(), sample.raw.ds_scratchpad.size()) ||
        !writer.u8(sample.raw.ds_resolution_bits) ||
        !writer.u8(sample.converted.primary_available ? 1U : 0U) ||
        !writer.f32(sample.converted.primary_value) ||
        !writer.u8(static_cast<uint8_t>(sample.converted.primary_kind)) ||
        !writer.u8(sample.converted.secondary_available ? 1U : 0U) ||
        !writer.f32(sample.converted.secondary_value) ||
        !writer.u8(static_cast<uint8_t>(sample.converted.secondary_kind)) ||
        !writer.u8(sample.header.profile_ref.present ? 1U : 0U) ||
        !writer.u8(static_cast<uint8_t>(sample.header.profile_ref.kind)) ||
        !writer.u32(sample.header.profile_ref.schema_version) ||
        !writer.u32(sample.header.profile_ref.profile_version) ||
        !writer.u32(sample.header.profile_ref.target_id) ||
        !writer.bytes(sample.header.profile_ref.hash.data(), sample.header.profile_ref.hash.size()) ||
        !writer.u8(sample.gas_ppm_available ? 1U : 0U) ||
        !writer.u8(0xa1U) || !writer.u8(1U) ||
        !writer.u8(sample.raw.heater_phase_available ? 1U : 0U) ||
        !writer.u8(static_cast<uint8_t>(sample.raw.heater_phase)) ||
        !writer.u8(sample.raw.sht31_ticks_available ? 1U : 0U) ||
        !writer.u16(sample.raw.sht31_temperature_ticks) ||
        !writer.u16(sample.raw.sht31_humidity_ticks)) return false;
    return finish(output, writer);
  }

  static bool decode_sample(const ImmutableRecordBytes& record,
                            acquisition::SampleRecord& sample) noexcept {
    ArchiveRecordView view{};
    if (!read(record, view) ||
        view.identity.kind != ArchiveRecordKind::Sample) {
      return false;
    }
    acquisition::SampleRecord output{};
    output.header.device_id = view.identity.device_id;
    output.header.boot_id = view.identity.boot_id;
    output.header.sequence = view.identity.sequence;
    PayloadReader reader{view.payload, view.payload_size};
    uint8_t sensor{0U};
    uint8_t stream{0U};
    uint8_t kind{0U};
    uint8_t quality{0U};
    uint8_t reason{0U};
    uint8_t flag{0U};
    uint8_t time_quality{0U};
    uint8_t primary_kind{0U};
    uint8_t secondary_kind{0U};
    uint8_t profile_kind{0U};
    if (!reader.u8(sensor) || !reader.u8(stream) || !reader.u8(kind) ||
        !reader.u64(output.header.monotonic_us) ||
        !reader.u8(quality) || !reader.u8(reason) ||
        !reader.u8(flag) || !reader.i64(output.header.recorded_at.epoch_ms)) {
      return false;
    }
    output.header.recorded_at.available = flag != 0U;
    if (!reader.u8(time_quality) ||
        !reader.u32(output.header.config_ref.schema_version) ||
        !reader.u64(output.header.config_ref.config_version) ||
        !reader.u8(flag) ||
        !reader.bytes(output.header.config_ref.hash.data(),
                      output.header.config_ref.hash.size())) {
      return false;
    }
    output.header.sensor = static_cast<acquisition::SensorId>(sensor);
    output.header.stream = static_cast<acquisition::StreamId>(stream);
    output.header.kind = static_cast<acquisition::SampleKind>(kind);
    output.quality = static_cast<acquisition::SampleQuality>(quality);
    output.reason = static_cast<acquisition::SampleReason>(reason);
    output.header.time_quality =
        static_cast<acquisition::TimeQuality>(time_quality);
    output.header.config_ref.present = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.raw.present = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.raw.crc_valid = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.raw.conversion_complete = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.raw.identity_valid = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.raw.temperature_crc_valid = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.raw.humidity_crc_valid = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.raw.heater_ready = flag != 0U;
    if (!reader.i64(output.raw.signed_counts) ||
        !reader.u16(output.raw.adc_counts) ||
        !reader.u32(output.raw.distance_us) ||
        !reader.i32(output.raw.temperature_centi_c) ||
        !reader.i32(output.raw.humidity_centi_pct) ||
        !reader.u8(flag) ||
        !reader.bytes(output.raw.ds_rom.data(), output.raw.ds_rom.size()) ||
        !reader.bytes(output.raw.ds_scratchpad.data(), output.raw.ds_scratchpad.size()) ||
        !reader.u8(output.raw.ds_resolution_bits)) return false;
    output.raw.ds_wire_evidence = flag != 0U;
    if (!reader.u8(flag)) return false;
    output.converted.primary_available = flag != 0U;
    if (!reader.f32(output.converted.primary_value) ||
        !reader.u8(primary_kind) || !reader.u8(flag)) return false;
    output.converted.primary_kind =
        static_cast<acquisition::ConvertedKind>(primary_kind);
    output.converted.secondary_available = flag != 0U;
    if (!reader.f32(output.converted.secondary_value) ||
        !reader.u8(secondary_kind) || !reader.u8(flag)) return false;
    output.converted.secondary_kind =
        static_cast<acquisition::ConvertedKind>(secondary_kind);
    output.header.profile_ref.present = flag != 0U;
    if (!reader.u8(profile_kind) ||
        !reader.u32(output.header.profile_ref.schema_version) ||
        !reader.u32(output.header.profile_ref.profile_version) ||
        !reader.u32(output.header.profile_ref.target_id) ||
        !reader.bytes(output.header.profile_ref.hash.data(),
                      output.header.profile_ref.hash.size()) ||
        !reader.u8(flag)) {
      return false;
    }
    output.header.profile_ref.kind =
        static_cast<acquisition::ProfileKind>(profile_kind);
    output.gas_ppm_available = flag != 0U;
    if (!reader.finished()) {
      uint8_t marker{0U}, extension{0U}, available{0U};
      if (!reader.u8(marker) || marker != 0xa1U || !reader.u8(extension) ||
          extension != 1U || !reader.u8(available)) return false;
      if (available > 1U) return false;
      output.raw.heater_phase_available = available != 0U;
      uint8_t phase{0U};
      if (!reader.u8(phase) || phase > static_cast<uint8_t>(acquisition::Mq7HeaterPhase::Fault) ||
          !reader.u8(available) || available > 1U ||
          !reader.u16(output.raw.sht31_temperature_ticks) ||
          !reader.u16(output.raw.sht31_humidity_ticks) || !reader.finished()) return false;
      output.raw.heater_phase = static_cast<acquisition::Mq7HeaterPhase>(phase);
      output.raw.sht31_ticks_available = available != 0U;
    }
    sample = output;
    return true;
  }

 private:
  static bool read_config_ref(PayloadReader& reader,
                              DecodedConfigRef& output) noexcept {
    uint8_t flag{0U};
    if (!reader.u32(output.schema_version) ||
        !reader.u64(output.config_version) || !reader.u8(flag) ||
        !reader.bytes(output.hash.data(), output.hash.size())) {
      return false;
    }
    output.present = flag != 0U;
    return true;
  }

  static bool read_profile_ref(PayloadReader& reader,
                               DecodedProfileRef& output) noexcept {
    uint8_t flag{0U};
    if (!reader.u8(flag) || !reader.u8(output.kind) ||
        !reader.u32(output.schema_version) ||
        !reader.u32(output.profile_version) ||
        !reader.u32(output.target_id) ||
        !reader.bytes(output.hash.data(), output.hash.size())) {
      return false;
    }
    output.present = flag != 0U;
    return true;
  }

  static void put_u16(uint8_t* out, uint16_t value) noexcept {
    out[0U] = static_cast<uint8_t>(value); out[1U] = static_cast<uint8_t>(value >> 8U);
  }
  static void put_u32(uint8_t* out, uint32_t value) noexcept {
    for (std::size_t i = 0U; i < 4U; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8U));
  }
  static void put_u64(uint8_t* out, uint64_t value) noexcept {
    for (std::size_t i = 0U; i < 8U; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8U));
  }
  static uint64_t get_u64(const uint8_t* in) noexcept {
    uint64_t value{0U};
    for (std::size_t i = 0U; i < 8U; ++i) {
      value |= static_cast<uint64_t>(in[i]) << (i * 8U);
    }
    return value;
  }
  static uint16_t get_u16(const uint8_t* in) noexcept {
    return static_cast<uint16_t>(in[0U] | (static_cast<uint16_t>(in[1U]) << 8U));
  }
  static uint32_t get_u32(const uint8_t* in) noexcept {
    uint32_t value{0U}; for (std::size_t i = 0U; i < 4U; ++i) value |= static_cast<uint32_t>(in[i]) << (i * 8U); return value;
  }
};

static_assert(kArchiveHeaderBytes + kArchiveTrailerBytes < kArchiveMaxRecordBytes,
              "archive framing must leave payload capacity");
static_assert(kArchiveHeaderCrcOffset + sizeof(uint32_t) == kArchiveHeaderBytes,
              "archive header field layout must remain exact");
static_assert(kArchiveDeviceIdOffset + 16U == kArchiveBootIdOffset,
              "archive identity offsets must remain contiguous");
static_assert(kArchiveBootIdOffset + 16U == kArchivePayloadCrcOffset,
              "archive boot identity offset must remain exact");

}  // namespace sigap::storage
