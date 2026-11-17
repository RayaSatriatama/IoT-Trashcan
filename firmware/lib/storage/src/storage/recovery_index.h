#pragma once
#ifdef ARDUINO
#include <cstdint>
#include <limits>
#include <esp_heap_caps.h>
#include "archive_codec.h"

namespace sigap::storage {
// Temporary lookup filter only. Every fingerprint match is checked against
// the complete, already validated on-disk frame by the caller.
class RecoveryPrefixIndex final {
 public:
  enum class Match { Other, Exact, Conflict, Error };
  enum class Result { Unique, Duplicate, Conflict, Error };
  explicit RecoveryPrefixIndex(std::size_t file_bytes) noexcept {
    if (file_bytes >= std::numeric_limits<uint32_t>::max()) return;
    const std::size_t frames = file_bytes / (kArchiveHeaderBytes + kArchiveTrailerBytes);
    const std::size_t target = 2U * (frames + 1U);
    capacity_ = 2U;
    while (capacity_ < target) {
      if (capacity_ > std::numeric_limits<std::size_t>::max() / 2U) { capacity_ = 0U; return; }
      capacity_ *= 2U;
    }
    if (capacity_ > std::numeric_limits<std::size_t>::max() / sizeof(Slot)) { capacity_ = 0U; return; }
    slots_ = static_cast<Slot*>(heap_caps_calloc(capacity_, sizeof(Slot),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  ~RecoveryPrefixIndex() { if (slots_ != nullptr) heap_caps_free(slots_); }
  RecoveryPrefixIndex(const RecoveryPrefixIndex&) = delete;
  RecoveryPrefixIndex& operator=(const RecoveryPrefixIndex&) = delete;
  bool active() const noexcept { return slots_ != nullptr; }
  std::size_t capacity() const noexcept { return active() ? capacity_ : 0U; }
  std::size_t bytes() const noexcept { return capacity() * sizeof(Slot); }

  template <typename Verify>
  Result observe(const ArchiveIdentity& identity, std::size_t offset, Verify verify) noexcept {
    if (!active() || offset >= std::numeric_limits<uint32_t>::max()) return Result::Error;
    const uint64_t key = fingerprint(identity);
    std::size_t position = static_cast<std::size_t>(key) & (capacity_ - 1U);
    for (std::size_t attempts = 0U; attempts < capacity_; ++attempts) {
      auto& slot = slots_[position];
      if (slot.offset_plus_one == 0U) {
        slot.fingerprint = key;
        slot.offset_plus_one = static_cast<uint32_t>(offset) + 1U;
        return Result::Unique;
      }
      if (slot.fingerprint == key) {
        switch (verify(slot.offset_plus_one - 1U)) {
          case Match::Exact: return Result::Duplicate;
          case Match::Conflict: return Result::Conflict;
          case Match::Error: return Result::Error;
          case Match::Other: break;
        }
      }
      position = (position + 1U) & (capacity_ - 1U);
    }
    return Result::Error;
  }

 private:
  struct Slot { uint64_t fingerprint; uint32_t offset_plus_one; };
  static uint64_t fingerprint(const ArchiveIdentity& identity) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    const auto add = [&hash](uint8_t value) noexcept { hash = (hash ^ value) * 1099511628211ULL; };
    for (const auto value : identity.device_id.bytes) add(value);
    for (const auto value : identity.boot_id.bytes) add(value);
    add(static_cast<uint8_t>(identity.kind));
    for (unsigned i = 0U; i < 8U; ++i) add(static_cast<uint8_t>(identity.sequence >> (i * 8U)));
    return hash;
  }
  Slot* slots_{nullptr};
  std::size_t capacity_{0U};
};
}  // namespace sigap::storage
#endif
