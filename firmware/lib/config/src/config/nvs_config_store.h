#pragma once

#include "config_store.h"

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace sigap::config {

class NvsConfigStore final : public IConfigSlotStore {
 public:
  NvsConfigStore() = default;

  bool read_active_pointer(ConfigSlot& slot) const noexcept override;
  bool read_slot(ConfigSlot slot,
                 EncodedConfig& encoded) const noexcept override;
  bool write_slot(ConfigSlot slot,
                  const EncodedConfig& encoded) noexcept override;
  bool write_active_pointer(ConfigSlot slot) noexcept override;
  bool is_provisioned() const noexcept override;
  bool mark_provisioned() noexcept override;

 private:
#ifdef ARDUINO
  static const char* slot_namespace(ConfigSlot slot) noexcept;
  mutable Preferences preferences_{};
#endif
};

}  // namespace sigap::config
