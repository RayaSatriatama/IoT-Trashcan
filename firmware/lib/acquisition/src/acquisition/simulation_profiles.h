#pragma once

#include "acquisition/profile_store.h"
#include "acquisition/target_protocol.h"

namespace sigap::acquisition {

// Protocol binding for the virtual DS device only. It is never written to a
// profile store, never a calibration result, and has no physical wire evidence.
inline bool make_simulated_ds18b20_profile(Ds18b20Profile& profile) noexcept {
  profile = Ds18b20Profile{};
  profile.manifest.schema_version = 1U;
  profile.manifest.profile_version = 1U;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  profile.manifest.validity = ProfileValidity::TestOnly;
  profile.has_rom = true;
  profile.rom = {0x28U, 0x53U, 0x49U, 0x4dU, 0U, 0U, 1U, 0U};
  profile.rom[7] = ds18b20_crc8(profile.rom.data(), 7U);
  profile.resolution_bits = 12U;
  EncodedProfile encoded{};
  const ProfileCodec codec{};
  if (!codec.encode(profile, encoded) ||
      !codec.validate(encoded, ProfileTarget::Ds18b20B3)) return false;
  profile.manifest.hash = encoded.hash();
  return true;
}

}  // namespace sigap::acquisition
