#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "acquisition/acquisition_types.h"

namespace sigap::acquisition {

enum class HandoffStatus : uint8_t { Enqueued, Dequeued, Empty, Full };

template <std::size_t Capacity>
class SampleRing final {
  static_assert(Capacity > 0U, "ring capacity must be nonzero");
 public:
  HandoffStatus try_push(const SampleRecord& record) noexcept {
    if (size_ == Capacity) {
      ++dropped_records_;
      return HandoffStatus::Full;
    }
    records_[tail_] = record;
    tail_ = (tail_ + 1U) % Capacity;
    ++size_;
    return HandoffStatus::Enqueued;
  }

  HandoffStatus try_pop(SampleRecord& record) noexcept {
    if (size_ == 0U) return HandoffStatus::Empty;
    record = records_[head_];
    head_ = (head_ + 1U) % Capacity;
    --size_;
    return HandoffStatus::Dequeued;
  }

  uint64_t dropped_records() const noexcept { return dropped_records_; }
  std::size_t size() const noexcept { return size_; }

 private:
  std::array<SampleRecord, Capacity> records_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
  uint64_t dropped_records_{0};
};

}  // namespace sigap::acquisition
