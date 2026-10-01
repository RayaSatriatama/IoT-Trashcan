#include "config_codec.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace sigap::config {

namespace {

class Sha256 final {
 public:
  Sha256() noexcept
      : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
               0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

  void update(const uint8_t* data, std::size_t size) noexcept {
    while (size != 0) {
      const std::size_t available = buffer_.size() - buffer_size_;
      const std::size_t count = std::min(available, size);
      std::copy_n(data, count, buffer_.begin() + buffer_size_);
      buffer_size_ += count;
      data += count;
      size -= count;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_.data());
        bit_length_ += 512;
        buffer_size_ = 0;
      }
    }
  }

  ConfigHash finish() noexcept {
    const uint64_t message_bits = bit_length_ + buffer_size_ * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      std::fill(buffer_.begin() + buffer_size_, buffer_.end(), 0U);
      transform(buffer_.data());
      buffer_size_ = 0;
    }
    std::fill(buffer_.begin() + buffer_size_, buffer_.begin() + 56U, 0U);
    for (std::size_t i = 0; i < sizeof(message_bits); ++i) {
      buffer_[63U - i] = static_cast<uint8_t>(message_bits >> (i * 8U));
    }
    transform(buffer_.data());

    ConfigHash result{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
      result[i * 4U] = static_cast<uint8_t>(state_[i] >> 24U);
      result[i * 4U + 1U] = static_cast<uint8_t>(state_[i] >> 16U);
      result[i * 4U + 2U] = static_cast<uint8_t>(state_[i] >> 8U);
      result[i * 4U + 3U] = static_cast<uint8_t>(state_[i]);
    }
    return result;
  }

 private:
  static constexpr std::array<uint32_t, 64> kRoundConstants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  static constexpr uint32_t rotate_right(uint32_t value,
                                          uint32_t count) noexcept {
    return (value >> count) | (value << (32U - count));
  }

  static constexpr uint32_t choose(uint32_t e, uint32_t f,
                                   uint32_t g) noexcept {
    return (e & f) ^ (~e & g);
  }

  static constexpr uint32_t majority(uint32_t a, uint32_t b,
                                     uint32_t c) noexcept {
    return (a & b) ^ (a & c) ^ (b & c);
  }

  static constexpr uint32_t big_sigma0(uint32_t value) noexcept {
    return rotate_right(value, 2U) ^ rotate_right(value, 13U) ^
           rotate_right(value, 22U);
  }

  static constexpr uint32_t big_sigma1(uint32_t value) noexcept {
    return rotate_right(value, 6U) ^ rotate_right(value, 11U) ^
           rotate_right(value, 25U);
  }

  static constexpr uint32_t small_sigma0(uint32_t value) noexcept {
    return rotate_right(value, 7U) ^ rotate_right(value, 18U) ^ (value >> 3U);
  }

  static constexpr uint32_t small_sigma1(uint32_t value) noexcept {
    return rotate_right(value, 17U) ^ rotate_right(value, 19U) ^
           (value >> 10U);
  }

  void transform(const uint8_t* block) noexcept {
    std::array<uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16U; ++i) {
      schedule[i] = (static_cast<uint32_t>(block[i * 4U]) << 24U) |
                    (static_cast<uint32_t>(block[i * 4U + 1U]) << 16U) |
                    (static_cast<uint32_t>(block[i * 4U + 2U]) << 8U) |
                    static_cast<uint32_t>(block[i * 4U + 3U]);
    }
    for (std::size_t i = 16U; i < schedule.size(); ++i) {
      schedule[i] = small_sigma1(schedule[i - 2U]) + schedule[i - 7U] +
                    small_sigma0(schedule[i - 15U]) + schedule[i - 16U];
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (std::size_t i = 0; i < schedule.size(); ++i) {
      const uint32_t t1 = h + big_sigma1(e) + choose(e, f, g) +
                          kRoundConstants[i] + schedule[i];
      const uint32_t t2 = big_sigma0(a) + majority(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<uint32_t, 8> state_{};
  std::array<uint8_t, 64> buffer_{};
  std::size_t buffer_size_{0};
  uint64_t bit_length_{0};
};

class Writer final {
 public:
  explicit Writer(std::array<uint8_t, kEncodedConfigBytes>& bytes) noexcept
      : bytes_(bytes) {}

  bool put_u16(uint16_t value) noexcept {
    if (!can_write(sizeof(value))) return false;
    bytes_[offset_++] = static_cast<uint8_t>(value);
    bytes_[offset_++] = static_cast<uint8_t>(value >> 8U);
    return true;
  }

  bool put_u32(uint32_t value) noexcept {
    if (!can_write(sizeof(value))) return false;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      bytes_[offset_++] = static_cast<uint8_t>(value >> (i * 8U));
    }
    return true;
  }

  bool put_u64(uint64_t value) noexcept {
    if (!can_write(sizeof(value))) return false;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      bytes_[offset_++] = static_cast<uint8_t>(value >> (i * 8U));
    }
    return true;
  }

  std::size_t offset() const noexcept { return offset_; }

 private:
  bool can_write(std::size_t count) const noexcept {
    return offset_ + count <= kConfigEnvelopeBytes;
  }

  std::array<uint8_t, kEncodedConfigBytes>& bytes_;
  std::size_t offset_{0};
};

class Reader final {
 public:
  Reader(const uint8_t* bytes, std::size_t size) noexcept
      : bytes_(bytes), size_(size) {}

  bool get_u16(uint16_t& value) noexcept {
    if (!can_read(sizeof(value))) return false;
    value = static_cast<uint16_t>(bytes_[offset_]) |
            (static_cast<uint16_t>(bytes_[offset_ + 1U]) << 8U);
    offset_ += sizeof(value);
    return true;
  }

  bool get_u32(uint32_t& value) noexcept {
    if (!can_read(sizeof(value))) return false;
    value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      value |= static_cast<uint32_t>(bytes_[offset_++]) << (i * 8U);
    }
    return true;
  }

  bool get_u64(uint64_t& value) noexcept {
    if (!can_read(sizeof(value))) return false;
    value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      value |= static_cast<uint64_t>(bytes_[offset_++]) << (i * 8U);
    }
    return true;
  }

  std::size_t offset() const noexcept { return offset_; }

 private:
  bool can_read(std::size_t count) const noexcept {
    return bytes_ != nullptr && offset_ + count <= size_;
  }

  const uint8_t* bytes_;
  std::size_t size_;
  std::size_t offset_{0};
};

void append_hash(std::array<uint8_t, kEncodedConfigBytes>& bytes,
                 const ConfigHash& hash) noexcept {
  std::copy(hash.begin(), hash.end(), bytes.begin() + kConfigEnvelopeBytes);
}

ConfigHash calculate_hash(const uint8_t* bytes) noexcept {
  Sha256 sha;
  sha.update(bytes, kConfigEnvelopeBytes);
  return sha.finish();
}

bool hashes_equal(const uint8_t* bytes, const ConfigHash& expected) noexcept {
  const ConfigHash actual = calculate_hash(bytes);
  return actual == expected;
}

}  // namespace

ConfigHash EncodedConfig::hash() const noexcept {
  ConfigHash result{};
  if (size < kConfigEnvelopeBytes + kConfigHashBytes) {
    return result;
  }
  std::copy_n(bytes.begin() + kConfigEnvelopeBytes, kConfigHashBytes,
              result.begin());
  return result;
}

bool ConfigCodec::encode(const AppConfig& config,
                         EncodedConfig& output) const noexcept {
  output = EncodedConfig{};
  Writer writer(output.bytes);
  if (!writer.put_u32(kConfigMagic) ||
      !writer.put_u16(kConfigFormatVersion) ||
      !writer.put_u32(config.schema_version) ||
      !writer.put_u64(config.config_version) ||
      !writer.put_u16(static_cast<uint16_t>(kConfigPayloadBytes)) ||
      !writer.put_u32(config.sampling.hx711_period_ms) ||
      !writer.put_u32(config.sampling.mq_period_ms) ||
      !writer.put_u32(config.sampling.ultrasonic_period_ms) ||
      !writer.put_u32(config.sampling.ds18b20_period_ms) ||
      !writer.put_u32(config.sampling.sht31_period_ms) ||
      !writer.put_u32(config.telemetry.summary_period_ms) ||
      !writer.put_u16(config.telemetry.max_records_per_batch) ||
      !writer.put_u32(config.telemetry.retry_min_ms) ||
      !writer.put_u32(config.telemetry.retry_max_ms) ||
      !writer.put_u64(config.storage.flash_pending_quota_bytes) ||
      !writer.put_u64(config.storage.sd_pending_quota_bytes) ||
      writer.offset() != kConfigEnvelopeBytes) {
    return false;
  }

  const ConfigHash hash = calculate_hash(output.bytes.data());
  append_hash(output.bytes, hash);
  output.size = kEncodedConfigBytes;
  return true;
}

bool ConfigCodec::decode(const uint8_t* bytes, std::size_t size,
                         AppConfig& output, ConfigHash& hash) const noexcept {
  if (bytes == nullptr || size != kEncodedConfigBytes) {
    return false;
  }

  std::copy_n(bytes + kConfigEnvelopeBytes, kConfigHashBytes, hash.begin());
  if (!hashes_equal(bytes, hash)) {
    return false;
  }

  Reader reader(bytes, kConfigEnvelopeBytes);
  uint32_t magic{0};
  uint16_t format_version{0};
  uint16_t payload_length{0};
  if (!reader.get_u32(magic) || !reader.get_u16(format_version) ||
      !reader.get_u32(output.schema_version) ||
      !reader.get_u64(output.config_version) ||
      !reader.get_u16(payload_length)) {
    return false;
  }
  if (magic != kConfigMagic || format_version != kConfigFormatVersion ||
      payload_length != kConfigPayloadBytes) {
    return false;
  }

  return reader.get_u32(output.sampling.hx711_period_ms) &&
         reader.get_u32(output.sampling.mq_period_ms) &&
         reader.get_u32(output.sampling.ultrasonic_period_ms) &&
         reader.get_u32(output.sampling.ds18b20_period_ms) &&
         reader.get_u32(output.sampling.sht31_period_ms) &&
         reader.get_u32(output.telemetry.summary_period_ms) &&
         reader.get_u16(output.telemetry.max_records_per_batch) &&
         reader.get_u32(output.telemetry.retry_min_ms) &&
         reader.get_u32(output.telemetry.retry_max_ms) &&
         reader.get_u64(output.storage.flash_pending_quota_bytes) &&
         reader.get_u64(output.storage.sd_pending_quota_bytes) &&
         reader.offset() == kConfigEnvelopeBytes;
}

bool ConfigCodec::decode(const EncodedConfig& encoded, AppConfig& output,
                         ConfigHash& hash) const noexcept {
  if (encoded.size != kEncodedConfigBytes) {
    return false;
  }
  return decode(encoded.bytes.data(), encoded.size, output, hash);
}

}  // namespace sigap::config
