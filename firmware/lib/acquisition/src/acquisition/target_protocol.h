#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sigap::acquisition {

// Dallas/Maxim CRC-8 used by DS18B20 ROM and scratchpad frames.
inline uint8_t ds18b20_crc8(const uint8_t* data, std::size_t size) noexcept {
  uint8_t crc{0U};
  for (std::size_t index = 0; index < size; ++index) {
    uint8_t value = static_cast<uint8_t>(crc ^ data[index]);
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      value = (value & 0x01U) != 0U
                  ? static_cast<uint8_t>((value >> 1U) ^ 0x8cU)
                  : static_cast<uint8_t>(value >> 1U);
    }
    crc = value;
  }
  return crc;
}

// The DS18B20 configuration register encodes resolution in bits 6:5. The
// lower bits in the values below preserve the protocol-defined reserved bits.
constexpr uint8_t ds18b20_resolution_config(
    uint8_t resolution_bits) noexcept {
  switch (resolution_bits) {
    case 9U: return 0x1fU;
    case 10U: return 0x3fU;
    case 11U: return 0x5fU;
    case 12U: return 0x7fU;
    default: return 0U;
  }
}

constexpr uint8_t ds18b20_resolution_from_config(
    uint8_t config) noexcept {
  switch (config & 0x60U) {
    case 0x00U: return 9U;
    case 0x20U: return 10U;
    case 0x40U: return 11U;
    case 0x60U: return 12U;
    default: return 0U;
  }
}

struct Ds18b20Scratchpad final {
  int16_t raw_temperature{0};
  uint8_t th{0U};
  uint8_t tl{0U};
  uint8_t config{0U};
  bool crc_valid{false};
};

inline bool parse_ds18b20_scratchpad(
    const std::array<uint8_t, 9>& scratchpad,
    Ds18b20Scratchpad& decoded) noexcept {
  decoded.raw_temperature = static_cast<int16_t>(
      static_cast<uint16_t>(scratchpad[0]) |
      (static_cast<uint16_t>(scratchpad[1]) << 8U));
  decoded.th = scratchpad[2];
  decoded.tl = scratchpad[3];
  decoded.config = scratchpad[4];
  decoded.crc_valid = ds18b20_crc8(scratchpad.data(), 8U) == scratchpad[8];
  return decoded.crc_valid;
}

inline bool ds18b20_resolution_matches(
    const Ds18b20Scratchpad& scratchpad,
    uint8_t expected_resolution_bits) noexcept {
  return scratchpad.crc_valid &&
         ds18b20_resolution_from_config(scratchpad.config) ==
             expected_resolution_bits;
}

inline bool decode_ds18b20_scratchpad(
    const std::array<uint8_t, 9>& scratchpad,
    int32_t& temperature_centi_c) noexcept {
  Ds18b20Scratchpad decoded{};
  if (!parse_ds18b20_scratchpad(scratchpad, decoded)) {
    return false;
  }

  // DS18B20 resolution is 1/16 C. Integer arithmetic keeps the record
  // representation deterministic and accepts a verified 85.00 C result.
  temperature_centi_c = static_cast<int32_t>(decoded.raw_temperature) * 625 / 100;
  return true;
}

// SHT31 CRC-8, polynomial 0x31, initial value 0xff.
inline uint8_t sht31_crc8(const uint8_t* data, std::size_t size) noexcept {
  uint8_t crc{0xffU};
  for (std::size_t index = 0; index < size; ++index) {
    crc = static_cast<uint8_t>(crc ^ data[index]);
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      crc = (crc & 0x80U) != 0U
                ? static_cast<uint8_t>((crc << 1U) ^ 0x31U)
                : static_cast<uint8_t>(crc << 1U);
    }
  }
  return crc;
}

struct Sht31DecodedFrame final {
  bool temperature_crc_valid{false};
  bool humidity_crc_valid{false};
  int32_t temperature_centi_c{0};
  int32_t humidity_centi_pct{0};
};

inline Sht31DecodedFrame decode_sht31_frame(
    const std::array<uint8_t, 6>& frame) noexcept {
  Sht31DecodedFrame decoded{};
  decoded.temperature_crc_valid =
      sht31_crc8(frame.data(), 2U) == frame[2];
  decoded.humidity_crc_valid =
      sht31_crc8(frame.data() + 3U, 2U) == frame[5];

  const uint16_t raw_temperature = static_cast<uint16_t>(
      (static_cast<uint16_t>(frame[0]) << 8U) | frame[1]);
  const uint16_t raw_humidity = static_cast<uint16_t>(
      (static_cast<uint16_t>(frame[3]) << 8U) | frame[4]);
  decoded.temperature_centi_c = static_cast<int32_t>(
      (-4500LL * 65535LL + 17500LL * raw_temperature) / 65535LL);
  decoded.humidity_centi_pct = static_cast<int32_t>(
      (10000LL * raw_humidity) / 65535LL);
  return decoded;
}

}  // namespace sigap::acquisition
