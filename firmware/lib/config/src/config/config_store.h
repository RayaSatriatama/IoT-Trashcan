#pragma once

#include <array>
#include <cstdint>

#include "config_codec.h"

namespace sigap::config {

enum class StoreReadStatus : uint8_t {
  Found,
  NotFound,
  Error,
  Invalid,
};

enum class ProvisioningStatus : uint8_t {
  Provisioned,
  Unprovisioned,
  Error,
};

inline constexpr std::size_t kActivationMetadataBytes{88U};

struct ActivationMetadata {
  ConfigSlot active_slot{ConfigSlot::A};
  uint64_t active_version{0};
  ConfigHash active_hash{};
  bool has_previous{false};
  ConfigSlot previous_slot{ConfigSlot::A};
  uint64_t previous_version{0};
  ConfigHash previous_hash{};
};

class IConfigSlotStore {
 public:
  virtual ~IConfigSlotStore() = default;

  virtual StoreReadStatus read_activation_metadata(
      ActivationMetadata& metadata) const noexcept = 0;
  virtual StoreReadStatus read_slot(ConfigSlot slot,
                                     EncodedConfig& encoded) const noexcept = 0;
  virtual bool write_slot(ConfigSlot slot,
                          const EncodedConfig& encoded) noexcept = 0;
  virtual bool write_activation_metadata(
      const ActivationMetadata& metadata) noexcept = 0;
  virtual ProvisioningStatus provisioning_status() const noexcept = 0;
  virtual bool mark_provisioned() noexcept = 0;
};

class MemoryConfigStore final : public IConfigSlotStore {
 public:
  enum class Fault : uint8_t {
    WriteSlot,
    CorruptAfterWrite,
    ReadSlot,
    WriteActivation,
    WritePointer = WriteActivation,
    ReadActivation,
    ReadPointer = ReadActivation,
    MarkProvisioned,
    ReadProvisioned,
  };

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

  void set_provisioned(bool provisioned) noexcept {
    provisioned_ = provisioned;
    has_provisioning_marker_ = true;
  }
  void clear_provisioned_marker() noexcept {
    has_provisioning_marker_ = false;
    provisioned_ = false;
  }
  void clear_activation_metadata() noexcept { has_activation_metadata_ = false; }
  void corrupt_slot(ConfigSlot slot) noexcept;
  void fail_next(Fault fault) noexcept;
  bool slot_present(ConfigSlot slot) const noexcept;

 private:
  static std::size_t index(ConfigSlot slot) noexcept {
    return slot == ConfigSlot::A ? 0U : 1U;
  }

  std::array<EncodedConfig, 2> slots_{};
  std::array<bool, 2> slot_present_{false, false};
  ActivationMetadata activation_metadata_{};
  bool has_activation_metadata_{false};
  bool provisioned_{false};
  bool has_provisioning_marker_{false};
  mutable Fault pending_fault_{Fault::WriteSlot};
  mutable bool has_pending_fault_{false};
};

}  // namespace sigap::config
