#include "config_store.h"

#include <algorithm>

namespace sigap::config {

bool MemoryConfigStore::read_active_pointer(ConfigSlot& slot) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadPointer) {
    has_pending_fault_ = false;
    return false;
  }
  if (!has_active_pointer_) {
    return false;
  }
  slot = active_slot_;
  return true;
}

bool MemoryConfigStore::read_slot(ConfigSlot slot,
                                  EncodedConfig& encoded) const noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::ReadSlot) {
    has_pending_fault_ = false;
    return false;
  }
  const std::size_t slot_index = index(slot);
  if (!slot_present_[slot_index]) {
    return false;
  }
  encoded = slots_[slot_index];
  return true;
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

bool MemoryConfigStore::write_active_pointer(ConfigSlot slot) noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::WritePointer) {
    has_pending_fault_ = false;
    return false;
  }
  active_slot_ = slot;
  has_active_pointer_ = true;
  return true;
}

bool MemoryConfigStore::mark_provisioned() noexcept {
  if (has_pending_fault_ && pending_fault_ == Fault::MarkProvisioned) {
    has_pending_fault_ = false;
    return false;
  }
  provisioned_ = true;
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
