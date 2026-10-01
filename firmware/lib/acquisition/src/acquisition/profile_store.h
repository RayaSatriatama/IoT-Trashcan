#pragma once

#include "acquisition/profiles.h"

namespace sigap::acquisition {

enum class ProfileReadStatus : uint8_t { Found, NotFound, Invalid, Error };

class IActiveProfileStore {
 public:
  virtual ~IActiveProfileStore() = default;
  virtual ProfileReadStatus read(SensorId sensor, ProfileManifest& manifest) noexcept = 0;
};

class NoProfileStore final : public IActiveProfileStore {
 public:
  ProfileReadStatus read(SensorId, ProfileManifest&) noexcept override {
    return ProfileReadStatus::NotFound;
  }
};

}  // namespace sigap::acquisition
