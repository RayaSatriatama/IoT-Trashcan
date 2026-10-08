#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sigap::provisioning {

inline constexpr std::size_t kProvisioningSecretBytes{32U};

class IProvisioningSecretStore {
 public:
  virtual ~IProvisioningSecretStore() = default;
  virtual bool load_or_create(
      std::array<uint8_t, kProvisioningSecretBytes>& output) noexcept = 0;
};

// The setup secret is intentionally stored in a separate NVS namespace from
// the backend bearer token.  The BLE GATT characteristics are encrypted and
// authenticated by the ESP-IDF BLE stack; this secret is never exposed over
// status, advertising, or the Web Bluetooth protocol.
class NvsProvisioningSecretStore final : public IProvisioningSecretStore {
 public:
  bool load_or_create(
      std::array<uint8_t, kProvisioningSecretBytes>& output) noexcept override;
};

class MemoryProvisioningSecretStore final : public IProvisioningSecretStore {
 public:
  bool load_or_create(
      std::array<uint8_t, kProvisioningSecretBytes>& output) noexcept override;
  void fail_next() noexcept { fail_ = true; }

 private:
  std::array<uint8_t, kProvisioningSecretBytes> value_{};
  bool present_{false};
  bool fail_{false};
};

}  // namespace sigap::provisioning
