#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "config_types.h"
#include "sigap/build/build_config.h"

namespace sigap::config {

inline constexpr uint32_t kConfigMagic{0x53474631U};  // "SGF1"
inline constexpr uint16_t kConfigFormatVersion{1};
inline constexpr std::size_t kConfigPayloadBytes{50};
inline constexpr std::size_t kConfigEnvelopeBytes{20 + kConfigPayloadBytes};
inline constexpr std::size_t kEncodedConfigBytes{kConfigEnvelopeBytes +
                                                 kConfigHashBytes};

static_assert(kEncodedConfigBytes < sigap::build::kNvsSize,
              "encoded config must fit the configured NVS partition");

struct EncodedConfig {
  std::array<uint8_t, kEncodedConfigBytes> bytes{};
  std::size_t size{0};

  ConfigHash hash() const noexcept;
};

class ConfigCodec final {
 public:
  bool encode(const AppConfig& config, EncodedConfig& output) const noexcept;
  bool decode(const uint8_t* bytes, std::size_t size, AppConfig& output,
              ConfigHash& hash) const noexcept;
  bool decode(const EncodedConfig& encoded, AppConfig& output,
              ConfigHash& hash) const noexcept;
};

}  // namespace sigap::config
