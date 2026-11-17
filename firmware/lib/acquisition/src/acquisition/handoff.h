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
#if defined(SIGAP_FRESH_WEB_PRIORITY14) && SIGAP_FRESH_WEB_PRIORITY14
      // Full live queues preserve channel coverage, not every raw attempt.
      // Account the actual evicted record; durable archives are untouched.
      std::size_t victim = Capacity;
      for (std::size_t i = 0U; i < size_; ++i) {
        const auto position = (head_ + i) % Capacity;
        const auto& queued = records_[position].header;
        if (queued.sensor == record.header.sensor && queued.stream == record.header.stream) {
          victim = position;
          break;
        }
      }
      if (victim == Capacity) {
        for (std::size_t i = 0U; i < size_ && victim == Capacity; ++i) {
          const auto position = (head_ + i) % Capacity;
          for (std::size_t j = i + 1U; j < size_; ++j) {
            const auto other = (head_ + j) % Capacity;
            if (records_[position].header.sensor == records_[other].header.sensor &&
                records_[position].header.stream == records_[other].header.stream) {
              victim = position;
              break;
            }
          }
        }
      }
      if (victim != Capacity) {
        const auto& dropped = records_[victim];
        ++dropped_records_;
        ++dropped_pending_count_;
        if (!has_dropped_ || dropped.header.sequence < first_dropped_.header.sequence)
          first_dropped_ = dropped;
        if (!has_dropped_ || dropped.header.sequence > last_dropped_.header.sequence)
          last_dropped_ = dropped;
        has_dropped_ = true;
        records_[victim] = record;
        return HandoffStatus::Enqueued;
      }
#endif
      ++dropped_records_;
      ++dropped_pending_count_;
      if (!has_dropped_) first_dropped_ = record;
      last_dropped_ = record;
      has_dropped_ = true;
      return HandoffStatus::Full;
    }
    records_[tail_] = record;
    tail_ = (tail_ + 1U) % Capacity;
    ++size_;
    return HandoffStatus::Enqueued;
  }

  HandoffStatus try_pop(SampleRecord& record) noexcept {
    if (try_peek(record) == HandoffStatus::Empty) return HandoffStatus::Empty;
    (void)discard_front();
    return HandoffStatus::Dequeued;
  }

  HandoffStatus try_peek(SampleRecord& record) const noexcept {
    if (size_ == 0U) return HandoffStatus::Empty;
#if defined(SIGAP_FRESH_WEB_PRIORITY14) && SIGAP_FRESH_WEB_PRIORITY14
    std::size_t after_offset = Capacity;
    std::size_t minimum_offset = 0U;
    uint32_t after_key = UINT32_MAX;
    uint32_t minimum_key = UINT32_MAX;
    for (std::size_t i = 0U; i < size_; ++i) {
      const auto key = channel_key(records_[(head_ + i) % Capacity]);
      if (key < minimum_key) { minimum_key = key; minimum_offset = i; }
      if (key > last_channel_key_ && key < after_key) { after_key = key; after_offset = i; }
    }
    selected_offset_ = after_offset != Capacity ? after_offset : minimum_offset;
    selected_ = true;
    record = records_[(head_ + selected_offset_) % Capacity];
#else
    record = records_[head_];
#endif
    return HandoffStatus::Dequeued;
  }

  HandoffStatus discard_front() noexcept {
    if (size_ == 0U) return HandoffStatus::Empty;
#if defined(SIGAP_FRESH_WEB_PRIORITY14) && SIGAP_FRESH_WEB_PRIORITY14
    const std::size_t offset = selected_ && selected_offset_ < size_ ? selected_offset_ : 0U;
    last_channel_key_ = channel_key(records_[(head_ + offset) % Capacity]);
    for (std::size_t i = offset; i + 1U < size_; ++i)
      records_[(head_ + i) % Capacity] = records_[(head_ + i + 1U) % Capacity];
    tail_ = (tail_ + Capacity - 1U) % Capacity;
    selected_ = false;
#else
    head_ = (head_ + 1U) % Capacity;
#endif
    --size_;
    return HandoffStatus::Dequeued;
  }

  uint64_t dropped_records() const noexcept { return dropped_records_; }
  const SampleRecord* last_dropped() const noexcept {
    return has_dropped_ ? &last_dropped_ : nullptr;
  }
  const SampleRecord* first_dropped() const noexcept {
    return has_dropped_ ? &first_dropped_ : nullptr;
  }
  uint64_t dropped_pending_count() const noexcept {
    return has_dropped_ ? dropped_pending_count_ : 0U;
  }
  void clear_last_dropped() noexcept {
    has_dropped_ = false;
    dropped_pending_count_ = 0U;
  }
  std::size_t size() const noexcept { return size_; }
  std::size_t pending_for(SensorId sensor) const noexcept {
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < size_; ++i)
      if (records_[(head_ + i) % Capacity].header.sensor == sensor) ++count;
    return count;
  }

 private:
#if defined(SIGAP_FRESH_WEB_PRIORITY14) && SIGAP_FRESH_WEB_PRIORITY14
  static uint32_t channel_key(const SampleRecord& record) noexcept {
    return (static_cast<uint32_t>(record.header.sensor) << 8U) |
           static_cast<uint32_t>(record.header.stream);
  }
  uint32_t last_channel_key_{0U};
  mutable std::size_t selected_offset_{0U};
  mutable bool selected_{false};
#endif
  std::array<SampleRecord, Capacity> records_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
  uint64_t dropped_records_{0};
  uint64_t dropped_pending_count_{0};
  SampleRecord last_dropped_{};
  SampleRecord first_dropped_{};
  bool has_dropped_{false};
};

}  // namespace sigap::acquisition
