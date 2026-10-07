#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "models/model_profiles.h"

namespace sigap::models {

inline constexpr uint32_t kModelEnvelopeMagic{0x53474D31U};  // SGM1
inline constexpr uint16_t kModelEnvelopeFormatVersion{1U};
inline constexpr std::size_t kModelEnvelopeHeaderBytes{23U};
inline constexpr std::size_t kMaxModelPayloadBytes{1400U};
inline constexpr std::size_t kModelHashBytes{32U};
inline constexpr std::size_t kMaxEncodedModelBytes{
    kModelEnvelopeHeaderBytes + kMaxModelPayloadBytes + kModelHashBytes};

struct EncodedModelProfile final {
  std::array<uint8_t, kMaxEncodedModelBytes> bytes{};
  std::size_t size{0U};

  config::ConfigHash hash() const noexcept;
};

struct ModelProfileIdentity final {
  ModelProfileKey key{};
  uint32_t schema_version{0U};
  uint32_t profile_version{0U};
  acquisition::ProfileValidity validity{
      acquisition::ProfileValidity::Missing};
  ModelDeployment deployment{ModelDeployment::Unconfigured};
  config::ConfigHash hash{};
};

class ModelProfileCodec final {
 public:
  bool encode(const FisModelProfile& profile,
              EncodedModelProfile& output) const noexcept;
  bool encode(const SafetyModelProfile& profile,
              EncodedModelProfile& output) const noexcept;

  bool decode(const EncodedModelProfile& encoded,
              FisModelProfile& output) const noexcept;
  bool decode(const EncodedModelProfile& encoded,
              SafetyModelProfile& output) const noexcept;

  bool validate(const EncodedModelProfile& encoded,
                ModelProfileKey expected) const noexcept;
  bool identity(const EncodedModelProfile& encoded,
                ModelProfileIdentity& output) const noexcept;
};

}  // namespace sigap::models
