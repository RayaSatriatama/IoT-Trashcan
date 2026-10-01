#pragma once

#include <array>
#include <cstdint>

#include "config_codec.h"

namespace sigap::config {

class IConfigSlotStore {
 public:
  virtual ~IConfigSlotStore() = default;

  virtual bool read_active_pointer(ConfigSlot& slot) const noexcept = 0;
  virtual bool read_slot(ConfigSlot slot,
                         EncodedConfig& encoded) const noexcept = 0;
  virtual bool write_slot(ConfigSlot slot,
                          const EncodedConfig& encoded) noexcept = 0;
  virtual bool write_active_pointer(ConfigSlot slot) noexcept = 0;
  virtual bool is_provisioned() const noexcept = 0;
  virtual bool mark_provisioned() noexcept = 0;
};

class MemoryConfigStore final : public IConfigSlotStore {
 public:
  enum class Fault : uint8_t {
    WriteSlot,
    CorruptAfterWrite,
    ReadSlot,
    WritePointer,
    ReadPointer,
    MarkProvisioned,
  };

  bool read_active_pointer(ConfigSlot& slot) const noexcept override;
  bool read_slot(ConfigSlot slot,
                 EncodedConfig& encoded) const noexcept override;
  bool write_slot(ConfigSlot slot,
                  const EncodedConfig& encoded) noexcept override;
  bool write_active_pointer(ConfigSlot slot) noexcept override;
  bool is_provisioned() const noexcept override { return provisioned_; }
  bool mark_provisioned() noexcept override;

  void set_provisioned(bool provisioned) noexcept { provisioned_ = provisioned; }
  void clear_active_pointer() noexcept { has_active_pointer_ = false; }
  void corrupt_slot(ConfigSlot slot) noexcept;
  void fail_next(Fault fault) noexcept;
  bool slot_present(ConfigSlot slot) const noexcept;

 private:
  static std::size_t index(ConfigSlot slot) noexcept {
    return slot == ConfigSlot::A ? 0U : 1U;
  }

  std::array<EncodedConfig, 2> slots_{};
  std::array<bool, 2> slot_present_{false, false};
  ConfigSlot active_slot_{ConfigSlot::A};
  bool has_active_pointer_{false};
  bool provisioned_{false};
  mutable Fault pending_fault_{Fault::WriteSlot};
  mutable bool has_pending_fault_{false};
};

}  // namespace sigap::config
