#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "telemetry/contract.h"

namespace sigap::telemetry_contract {

// Caller-owned storage keeps the canonical writer bounded without adding a
// permanent 16 KiB object to the acquisition runtime.
class CanonicalBuffer final {
 public:
  CanonicalBuffer(char* data, std::size_t capacity) noexcept
      : data_{data}, capacity_{capacity}, size_{0U} {}

  bool append(std::string_view value) noexcept {
    if (value.size() > remaining()) {
      return false;
    }
    for (const char character : value) {
      data_[size_++] = character;
    }
    return true;
  }

  bool append(char value) noexcept {
    if (remaining() == 0U) {
      return false;
    }
    data_[size_++] = value;
    return true;
  }

  std::string_view view() const noexcept {
    return {data_, size_};
  }

  char* data() const noexcept { return data_; }

  std::size_t size() const noexcept { return size_; }

  std::size_t capacity() const noexcept { return capacity_; }

  bool set_size(std::size_t size) noexcept {
    if (size > capacity_) return false;
    size_ = size;
    return true;
  }

 private:
  std::size_t remaining() const noexcept {
    return capacity_ - size_;
  }

  char* data_;
  std::size_t capacity_;
  std::size_t size_;
};

struct Mq4SampleFields final {
  std::string_view device_id;
  std::string_view boot_id;
  uint64_t sequence{0U};
  uint64_t monotonic_us{0U};
  std::string_view config_hash;
  uint64_t config_version{0U};
  uint64_t config_schema_version{0U};
  uint64_t adc_counts{0U};
};

namespace detail {

inline bool append_uint64(CanonicalBuffer& out, uint64_t value) noexcept {
  char reverse[20]{};
  std::size_t count{0U};
  do {
    reverse[count++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value != 0U && count < sizeof(reverse));

  if (count == 0U || count > sizeof(reverse)) {
    return false;
  }
  for (std::size_t index = count; index > 0U; --index) {
    if (!out.append(reverse[index - 1U])) {
      return false;
    }
  }
  return true;
}

inline bool append_json_string_content(
    CanonicalBuffer& out,
    std::string_view value) noexcept {
  // IDs and config hashes are ASCII in this fixture. Rejecting malformed
  // UTF-8 here prevents the writer from emitting bytes that are not valid
  // JSON Unicode. The escape path also handles general UTF-8 content.
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const uint8_t byte = static_cast<uint8_t>(value[index]);
    if (byte < 0x20U) {
      switch (byte) {
        case '\b':
          if (!out.append("\\b")) return false;
          break;
        case '\f':
          if (!out.append("\\f")) return false;
          break;
        case '\n':
          if (!out.append("\\n")) return false;
          break;
        case '\r':
          if (!out.append("\\r")) return false;
          break;
        case '\t':
          if (!out.append("\\t")) return false;
          break;
        default: {
          constexpr char kHex[] = "0123456789abcdef";
          if (!out.append("\\u00")) return false;
          if (!out.append(kHex[byte >> 4U])) return false;
          if (!out.append(kHex[byte & 0x0fU])) return false;
          break;
        }
      }
      continue;
    }
    if (byte == '"') {
      if (!out.append("\\\"")) return false;
      continue;
    }
    if (byte == '\\') {
      if (!out.append("\\\\")) return false;
      continue;
    }
    if (byte >= 0x80U) {
      // Validate UTF-8 sequence shape and Unicode scalar restrictions. Raw
      // UTF-8 is the shortest JCS representation for non-ASCII characters.
      const std::size_t remaining = value.size() - index;
      std::size_t width = 0U;
      uint32_t code_point = 0U;
      if (byte <= 0xdfU && remaining >= 2U &&
          (static_cast<uint8_t>(value[index + 1U]) & 0xc0U) == 0x80U) {
        width = 2U;
        code_point = static_cast<uint32_t>(byte & 0x1fU) << 6U;
        code_point |= static_cast<uint32_t>(value[index + 1U] & 0x3fU);
        if (byte < 0xc2U) return false;
      } else if (byte >= 0xe0U && byte <= 0xefU && remaining >= 3U) {
        const uint8_t second = static_cast<uint8_t>(value[index + 1U]);
        const uint8_t third = static_cast<uint8_t>(value[index + 2U]);
        if ((second & 0xc0U) != 0x80U || (third & 0xc0U) != 0x80U ||
            (byte == 0xe0U && second < 0xa0U) ||
            (byte == 0xedU && second >= 0xa0U)) {
          return false;
        }
        width = 3U;
        code_point = static_cast<uint32_t>(byte & 0x0fU) << 12U;
        code_point |= static_cast<uint32_t>(second & 0x3fU) << 6U;
        code_point |= static_cast<uint32_t>(third & 0x3fU);
      } else if (byte >= 0xf0U && byte <= 0xf4U && remaining >= 4U) {
        const uint8_t second = static_cast<uint8_t>(value[index + 1U]);
        const uint8_t third = static_cast<uint8_t>(value[index + 2U]);
        const uint8_t fourth = static_cast<uint8_t>(value[index + 3U]);
        if ((second & 0xc0U) != 0x80U || (third & 0xc0U) != 0x80U ||
            (fourth & 0xc0U) != 0x80U ||
            (byte == 0xf0U && second < 0x90U) ||
            (byte == 0xf4U && second > 0x8fU)) {
          return false;
        }
        width = 4U;
        code_point = static_cast<uint32_t>(byte & 0x07U) << 18U;
        code_point |= static_cast<uint32_t>(second & 0x3fU) << 12U;
        code_point |= static_cast<uint32_t>(third & 0x3fU) << 6U;
        code_point |= static_cast<uint32_t>(fourth & 0x3fU);
      } else {
        return false;
      }
      if ((code_point >= 0xfdd0U && code_point <= 0xfdefU) ||
          (code_point & 0xffffU) == 0xfffeU ||
          (code_point & 0xffffU) == 0xffffU) {
        return false;
      }
      if (!out.append(value.substr(index, width))) return false;
      index += width - 1U;
      continue;
    }
    if (!out.append(value[index])) return false;
  }
  return true;
}

inline bool append_json_string(
    CanonicalBuffer& out,
    std::string_view value) noexcept {
  return out.append('"') && append_json_string_content(out, value) &&
         out.append('"');
}

inline bool append_quoted_key(
    CanonicalBuffer& out,
    std::string_view key) noexcept {
  return append_json_string(out, key) && out.append(':');
}

inline bool append_quoted_uint64(
    CanonicalBuffer& out,
    uint64_t value) noexcept {
  if (!out.append('"')) return false;
  if (!append_uint64(out, value)) return false;
  return out.append('"');
}

inline bool append_record_id(
    CanonicalBuffer& out,
    const Mq4SampleFields& record) noexcept {
  if (!out.append('"') ||
      !append_json_string_content(out, record.device_id) ||
      !out.append('/') ||
      !append_json_string_content(out, record.boot_id) ||
      !out.append('/') ||
      !append_uint64(out, record.sequence) ||
      !out.append('"')) {
    return false;
  }
  return true;
}

}  // namespace detail

// Writes the frozen MQ-4 uncalibrated sample fixture from typed semantic
// fields. Member order is the UTF-16 lexicographic order required by JCS.
// This intentionally covers the bounded sample shape used by the firmware
// contract proof, rather than pretending to be a general JSON serializer.
inline bool write_canonical_mq4_sample(
    const Mq4SampleFields& record,
    CanonicalBuffer& out) noexcept {
  if (record.device_id.empty() || record.boot_id.empty() ||
      record.config_hash.empty() || record.config_hash.front() != 's') {
    return false;
  }

  using detail::append_json_string;
  using detail::append_quoted_key;
  using detail::append_quoted_uint64;
  using detail::append_record_id;
  using detail::append_uint64;

  if (!out.append('{')) return false;
  if (!append_quoted_key(out, "boot_id") ||
      !append_json_string(out, record.boot_id) || !out.append(',')) return false;
  if (!append_quoted_key(out, "config_ref") || !out.append('{')) return false;
  if (!append_quoted_key(out, "config_hash") ||
      !append_json_string(out, record.config_hash) || !out.append(',')) return false;
  if (!append_quoted_key(out, "config_version") ||
      !append_quoted_uint64(out, record.config_version) || !out.append(',')) return false;
  if (!append_quoted_key(out, "schema_version") ||
      !append_uint64(out, record.config_schema_version) || !out.append('}') ||
      !out.append(',')) return false;
  if (!append_quoted_key(out, "converted") || !out.append("null") || !out.append(',')) return false;
  if (!append_quoted_key(out, "device_id") ||
      !append_json_string(out, record.device_id) || !out.append(',')) return false;
  if (!append_quoted_key(out, "kind") ||
      !append_json_string(out, "sample") || !out.append(',')) return false;
  if (!append_quoted_key(out, "monotonic_us") ||
      !append_quoted_uint64(out, record.monotonic_us) || !out.append(',')) return false;
  if (!append_quoted_key(out, "profile_refs") || !out.append('[') ||
      !out.append(']') || !out.append(',')) return false;
  if (!append_quoted_key(out, "quality") ||
      !append_json_string(out, "UNCALIBRATED") || !out.append(',')) return false;
  if (!append_quoted_key(out, "raw") || !out.append('{') ||
      !append_quoted_key(out, "adc_counts") ||
      !append_uint64(out, record.adc_counts) || !out.append('}') ||
      !out.append(',')) return false;
  if (!append_quoted_key(out, "reason") ||
      !append_json_string(out, "PROFILE_MISSING") || !out.append(',')) return false;
  if (!append_quoted_key(out, "record_id") ||
      !append_record_id(out, record) || !out.append(',')) return false;
  if (!append_quoted_key(out, "recorded_at") || !out.append("null") ||
      !out.append(',')) return false;
  if (!append_quoted_key(out, "sensor_id") ||
      !append_json_string(out, "mq4") || !out.append(',')) return false;
  if (!append_quoted_key(out, "sequence") ||
      !append_quoted_uint64(out, record.sequence) || !out.append(',')) return false;
  if (!append_quoted_key(out, "stream") ||
      !append_json_string(out, "organic") || !out.append(',')) return false;
  if (!append_quoted_key(out, "time_quality") ||
      !append_json_string(out, "UNSYNCED") || !out.append('}')) return false;
  return true;
}

inline std::array<uint8_t, 32> hash_canonical_mq4_sample(
    const Mq4SampleFields& record,
    char* storage,
    std::size_t capacity) noexcept {
  CanonicalBuffer out{storage, capacity};
  if (!write_canonical_mq4_sample(record, out)) {
    return {};
  }
  return sha256(out.view());
}

}  // namespace sigap::telemetry_contract
