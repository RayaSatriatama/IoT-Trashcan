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

inline bool decode_ds18b20_scratchpad(
    const std::array<uint8_t, 9>& scratchpad,
    int32_t& temperature_centi_c) noexcept {
  if (ds18b20_crc8(scratchpad.data(), 8U) != scratchpad[8]) {
    return false;
  }

  const int16_t raw = static_cast<int16_t>(
      static_cast<uint16_t>(scratchpad[0]) |
      (static_cast<uint16_t>(scratchpad[1]) << 8U));
  // DS18B20 resolution is 1/16 C. Integer arithmetic keeps the record
  // representation deterministic and accepts a verified 85.00 C result.
  temperature_centi_c = static_cast<int32_t>(raw) * 625 / 100;
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
