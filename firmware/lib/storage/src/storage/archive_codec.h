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
    output = ImmutableRecordBytes{};
    output.identity = identity;
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
        !writer.i64(sample.raw.signed_counts) || !writer.u16(sample.raw.adc_counts) ||
        !writer.u32(sample.raw.distance_us) || !writer.i32(sample.raw.temperature_centi_c) ||
        !writer.i32(sample.raw.humidity_centi_pct) ||
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
        !writer.u8(sample.gas_ppm_available ? 1U : 0U)) return false;
    return finish(output, writer);
  }

 private:
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
