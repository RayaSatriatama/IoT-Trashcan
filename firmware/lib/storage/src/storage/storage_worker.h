#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "archive_store.h"

namespace sigap::storage {

// A producer owns the immutable record bytes until the worker reports Durable
// or the terminal callback has recorded the loss.  The queue deliberately
// stores a small frozen reference instead of a 16 KiB frame per entry.
struct StorageWorkItem final {
  const ImmutableRecordBytes* record{nullptr};
  ArchiveIdentity identity{};
  config::ConfigHash hash{};
  void* terminal_context{nullptr};
  void (*terminal_gap)(void*, const ArchiveIdentity&, GapReason) noexcept{nullptr};
};

template <std::size_t Capacity>
class StorageWorkQueue final {
  static_assert(Capacity > 0U, "storage queue capacity must be nonzero");

 public:
  bool try_push(const StorageWorkItem& item) noexcept {
    if (item.record == nullptr || size_ == Capacity) {
      ++overflow_count_;
      return false;
    }
    items_[tail_] = item;
    tail_ = (tail_ + 1U) % Capacity;
    ++size_;
    if (size_ > high_water_) high_water_ = size_;
    return true;
  }

  StorageWorkItem* peek() noexcept {
    return size_ == 0U ? nullptr : &items_[head_];
  }

  const StorageWorkItem* peek() const noexcept {
    return size_ == 0U ? nullptr : &items_[head_];
  }

  bool discard_front() noexcept {
    if (size_ == 0U) return false;
    items_[head_] = StorageWorkItem{};
    head_ = (head_ + 1U) % Capacity;
    --size_;
    return true;
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return Capacity; }
  std::size_t high_water() const noexcept { return high_water_; }
  uint64_t overflow_count() const noexcept { return overflow_count_; }

 private:
  std::array<StorageWorkItem, Capacity> items_{};
  std::size_t head_{0U};
  std::size_t tail_{0U};
  std::size_t size_{0U};
  std::size_t high_water_{0U};
  uint64_t overflow_count_{0U};
};

struct StorageWorkerStats final {
  uint64_t enqueued{0U};
  uint64_t durable{0U};
  uint64_t retryable{0U};
  uint64_t terminal{0U};
  uint64_t invalid_source{0U};
  uint64_t overflow{0U};
  std::size_t high_water{0U};
};

// Single-owner storage worker.  A producer may enqueue from the acquisition
// boundary; only this worker calls the archive backend.  process() is bounded
// and leaves Retryable/LoggingBlocked items at the head for a later invocation.
template <std::size_t Capacity>
class StorageWorker final {
 public:
  explicit StorageWorker(IArchiveStore* store = nullptr) noexcept
      : store_(store) {}

  void set_store(IArchiveStore* store) noexcept { store_ = store; }

  bool try_enqueue(const ImmutableRecordBytes& record,
                   void* terminal_context = nullptr,
                   void (*terminal_gap)(void*, const ArchiveIdentity&,
                                        GapReason) noexcept = nullptr) noexcept {
    if (!RecordCodec::valid(record)) {
      ++stats_.invalid_source;
      if (terminal_gap != nullptr) {
        terminal_gap(terminal_context, record.identity, GapReason::InvalidRecord);
      }
      ++stats_.terminal;
      return false;
    }
    const StorageWorkItem item{&record, record.identity, record.hash,
                               terminal_context, terminal_gap};
    if (!queue_.try_push(item)) {
      ++stats_.overflow;
      return false;
    }
    ++stats_.enqueued;
    stats_.high_water = queue_.high_water();
    return true;
  }

  // Copies one encoded record into the worker-owned pending slot.  This is
  // the safe handoff for producers whose source handoff may be reused after
  // enqueue.  The slot is intentionally singular: upstream typed rings keep
  // ownership of all additional records and provide backpressure without a
  // second 16 KiB frame allocation per queue entry.
  bool try_enqueue_copy(
      const ImmutableRecordBytes& record, void* terminal_context = nullptr,
      void (*terminal_gap)(void*, const ArchiveIdentity&,
                           GapReason) noexcept = nullptr) noexcept {
    if (pending_owned_) {
      ++stats_.overflow;
      return false;
    }
    if (!RecordCodec::valid(record)) {
      ++stats_.invalid_source;
      if (terminal_gap != nullptr) {
        terminal_gap(terminal_context, record.identity, GapReason::InvalidRecord);
      }
      ++stats_.terminal;
      return false;
    }
    pending_record_ = record;
    pending_owned_ = true;
    const StorageWorkItem item{&pending_record_, pending_record_.identity,
                               pending_record_.hash, terminal_context,
                               terminal_gap};
    if (!queue_.try_push(item)) {
      pending_record_ = ImmutableRecordBytes{};
      pending_owned_ = false;
      ++stats_.overflow;
      return false;
    }
    ++stats_.enqueued;
    stats_.high_water = queue_.high_water();
    return true;
  }

  // Returns true when one item was removed or terminally discarded.  A
  // transient backend failure returns false and preserves the exact item.
  bool process_one() noexcept {
    StorageWorkItem* item = queue_.peek();
    if (item == nullptr || store_ == nullptr) return false;
    if (item->record == nullptr || !same_frozen_identity(*item)) {
      const bool owned = item->record == &pending_record_;
      terminal(*item, GapReason::InvalidRecord);
      queue_.discard_front();
      if (owned) clear_owned();
      ++stats_.terminal;
      ++stats_.invalid_source;
      return true;
    }

    const AppendStatus status = store_->append(*item->record);
    if (status == AppendStatus::Durable) {
      const bool owned = item->record == &pending_record_;
      queue_.discard_front();
      if (owned) clear_owned();
      ++stats_.durable;
      return true;
    }
    if (status == AppendStatus::Invalid) {
      const bool owned = item->record == &pending_record_;
      terminal(*item, GapReason::InvalidRecord);
      queue_.discard_front();
      if (owned) clear_owned();
      ++stats_.terminal;
      return true;
    }
    ++stats_.retryable;
    return false;
  }

  std::size_t process(std::size_t budget) noexcept {
    std::size_t completed{0U};
    while (completed < budget && process_one()) ++completed;
    return completed;
  }

  const StorageWorkQueue<Capacity>& queue() const noexcept { return queue_; }
  StorageWorkQueue<Capacity>& queue() noexcept { return queue_; }
  const StorageWorkerStats& stats() const noexcept { return stats_; }

 private:
  static bool same_identity(const ArchiveIdentity& left,
                            const ArchiveIdentity& right) noexcept {
    return left.kind == right.kind && left.sequence == right.sequence &&
           left.device_id.present == right.device_id.present &&
           left.device_id.bytes == right.device_id.bytes &&
           left.boot_id.present == right.boot_id.present &&
           left.boot_id.bytes == right.boot_id.bytes;
  }

  bool same_frozen_identity(const StorageWorkItem& item) const noexcept {
    return same_identity(item.record->identity, item.identity) &&
           item.record->hash == item.hash;
  }

  static void terminal(const StorageWorkItem& item, GapReason reason) noexcept {
    if (item.terminal_gap != nullptr) {
      item.terminal_gap(item.terminal_context, item.identity, reason);
    }
  }

  void clear_owned() noexcept {
    pending_record_ = ImmutableRecordBytes{};
    pending_owned_ = false;
  }

  IArchiveStore* store_{nullptr};
  StorageWorkQueue<Capacity> queue_{};
  ImmutableRecordBytes pending_record_{};
  bool pending_owned_{false};
  StorageWorkerStats stats_{};
};

}  // namespace sigap::storage
