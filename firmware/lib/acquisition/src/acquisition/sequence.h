#pragma once

#include <cstdint>

namespace sigap::acquisition {

class SequenceAllocator final {
 public:
  explicit SequenceAllocator(uint64_t next = 0U) noexcept : next_(next) {}

  bool allocate(uint64_t& value) noexcept {
    if (exhausted()) return false;
    value = next_++;
    return true;
  }
  uint64_t next_value() const noexcept { return next_; }
  bool exhausted() const noexcept { return next_ == UINT64_MAX; }

 private:
  uint64_t next_{0};
};

}  // namespace sigap::acquisition
