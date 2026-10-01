#pragma once

#include "config_store.h"

namespace sigap::config {

class NvsConfigStore final : public IConfigSlotStore {
 public:
  NvsConfigStore() = default;

  StoreReadStatus read_activation_metadata(
      ActivationMetadata& metadata) const noexcept override;
  StoreReadStatus read_slot(ConfigSlot slot,
                             EncodedConfig& encoded) const noexcept override;
  bool write_slot(ConfigSlot slot,
                  const EncodedConfig& encoded) noexcept override;
  bool write_activation_metadata(
      const ActivationMetadata& metadata) noexcept override;
  ProvisioningStatus provisioning_status() const noexcept override;
  bool mark_provisioned() noexcept override;

 private:
#ifdef ARDUINO
  static const char* slot_namespace(ConfigSlot slot) noexcept;
#endif
};

}  // namespace sigap::config
