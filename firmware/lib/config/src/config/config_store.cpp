#include "config_store.h"

#include <algorithm>

namespace sigap::config {

StoreReadStatus MemoryConfigStore::read_activation_metadata(
    ActivationMetadata& metadata) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadActivation) {
    has_pending_fault_ = false;
    return StoreReadStatus::Error;
  }
  if (!has_activation_metadata_) {
    return StoreReadStatus::NotFound;
  }
  metadata = activation_metadata_;
  return StoreReadStatus::Found;
}

StoreReadStatus MemoryConfigStore::read_slot(
    ConfigSlot slot, EncodedConfig& encoded) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadSlot) {
    has_pending_fault_ = false;
    return StoreReadStatus::Error;
  }
  const std::size_t slot_index = index(slot);
  if (!slot_present_[slot_index]) {
    return StoreReadStatus::NotFound;
  }
  encoded = slots_[slot_index];
  return StoreReadStatus::Found;
}

bool MemoryConfigStore::write_slot(ConfigSlot slot,
                                   const EncodedConfig& encoded) noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::WriteSlot) {
    has_pending_fault_ = false;
    return false;
  }
  const std::size_t slot_index = index(slot);
  slots_[slot_index] = encoded;
  slot_present_[slot_index] = true;
  if (has_pending_fault_ && pending_fault_ == Fault::CorruptAfterWrite) {
    slots_[slot_index].bytes[0] ^= 0xffU;
    has_pending_fault_ = false;
  }
  return true;
}

bool MemoryConfigStore::write_activation_metadata(
    const ActivationMetadata& metadata) noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::WriteActivation) {
    has_pending_fault_ = false;
    return false;
  }
  activation_metadata_ = metadata;
  has_activation_metadata_ = true;
  return true;
}

ProvisioningStatus MemoryConfigStore::provisioning_status() const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadProvisioned) {
    has_pending_fault_ = false;
    return ProvisioningStatus::Error;
  }
  if (!has_provisioning_marker_) return ProvisioningStatus::Unprovisioned;
  return provisioned_ ? ProvisioningStatus::Provisioned
                      : ProvisioningStatus::Unprovisioned;
}

bool MemoryConfigStore::mark_provisioned() noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::MarkProvisioned) {
    has_pending_fault_ = false;
    return false;
  }
  provisioned_ = true;
  has_provisioning_marker_ = true;
  return true;
}

void MemoryConfigStore::corrupt_slot(ConfigSlot slot) noexcept {
  const std::size_t slot_index = index(slot);
  if (slot_present_[slot_index]) {
    slots_[slot_index].bytes[0] ^= 0xffU;
  }
}

void MemoryConfigStore::fail_next(Fault fault) noexcept {
  pending_fault_ = fault;
  has_pending_fault_ = true;
}

bool MemoryConfigStore::slot_present(ConfigSlot slot) const noexcept {
  return slot_present_[index(slot)];
}

}  // namespace sigap::config
