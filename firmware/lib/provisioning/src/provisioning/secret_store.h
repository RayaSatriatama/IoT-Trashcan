#pragma once

#include <array>
#include <cstdint>

namespace sigap::provisioning {

struct NetworkSecrets {
  std::array<char, 33> ssid{};
  std::array<char, 65> password{};
  std::array<char, 129> device_token{};
};

struct SecretPresence {
  bool wifi_configured{false};
  bool device_token_configured{false};
};

class SecretStore {
 public:
  virtual ~SecretStore() = default;

  // Only internal network/auth services may receive plaintext secrets.
  virtual bool load_for_network(NetworkSecrets& output) = 0;
  virtual SecretPresence status() const noexcept = 0;

  // Provisioning and clear operations require an explicit maintenance layer.
  virtual bool set_wifi(const char* ssid, const char* password) = 0;
  virtual bool rotate_device_token(const char* token) = 0;
  virtual bool clear_all(bool physical_confirmation) = 0;
};

}  // namespace sigap::provisioning
