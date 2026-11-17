#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "archive_codec.h"
#include "config/config_types.h"

namespace sigap::storage {

// Queued means ownership transferred to the instance-owned storage worker.
// It is deliberately distinct from Durable: a producer may release its
// source handoff only after Queued, while the worker still retries the frozen
// bytes until the backend reports Durable.
enum class AppendStatus : uint8_t {
  Durable,
  Queued,
  Retryable,
  LoggingBlocked,
  Invalid
};
enum class ArchiveHealth : uint8_t { Healthy, Degraded, Blocked };
enum class GapReason : uint8_t { QueueFull, QuotaFull, MediaFailure, InvalidRecord };

struct GapRange final {
  uint64_t first_sequence{0U};
  uint64_t last_sequence{0U};
  acquisition::DeviceId device_id{};
  acquisition::BootId boot_id{};
  acquisition::SensorId sensor{acquisition::SensorId::Mq4};
  acquisition::StreamId stream{acquisition::StreamId::Organic};
  uint64_t first_monotonic_us{0U};
  uint64_t last_monotonic_us{0U};
  uint64_t record_count{0U};
  GapReason reason{GapReason::QueueFull};
  bool present{false};
};

template <std::size_t Capacity>
class GapTracker final {
 public:
  bool record(uint64_t sequence, GapReason reason) noexcept {
    ++unlogged_records_;
    if (count_ != 0U && ranges_[count_ - 1U].reason == reason &&
        ranges_[count_ - 1U].last_sequence != UINT64_MAX &&
        ranges_[count_ - 1U].last_sequence + 1U == sequence) {
      ranges_[count_ - 1U].last_sequence = sequence;
      ++ranges_[count_ - 1U].record_count;
      return true;
    }
    if (count_ == Capacity) {
      ++overflow_count_;
      return false;
    }
    GapRange range{};
    range.first_sequence = sequence;
    range.last_sequence = sequence;
    range.record_count = 1U;
    range.reason = reason;
    range.present = true;
    ranges_[count_++] = range;
    return true;
  }
  bool record(const acquisition::SampleRecord& sample,
              GapReason reason) noexcept {
    ++unlogged_records_;
    if (count_ != 0U) {
      GapRange& current = ranges_[count_ - 1U];
      if (current.reason == reason && current.device_id.bytes == sample.header.device_id.bytes &&
          current.boot_id.bytes == sample.header.boot_id.bytes &&
          current.sensor == sample.header.sensor && current.stream == sample.header.stream &&
          current.last_sequence != UINT64_MAX &&
          current.last_sequence + 1U == sample.header.sequence) {
        current.last_sequence = sample.header.sequence;
        current.last_monotonic_us = sample.header.monotonic_us;
        ++current.record_count;
        return true;
      }
    }
    if (count_ == Capacity) {
      ++overflow_count_;
      return false;
    }
    GapRange range{};
    range.first_sequence = sample.header.sequence;
    range.last_sequence = sample.header.sequence;
    range.device_id = sample.header.device_id;
    range.boot_id = sample.header.boot_id;
    range.sensor = sample.header.sensor;
    range.stream = sample.header.stream;
    range.first_monotonic_us = sample.header.monotonic_us;
    range.last_monotonic_us = sample.header.monotonic_us;
    range.record_count = 1U;
    range.reason = reason;
    range.present = true;
    ranges_[count_++] = range;
    return true;
  }
  bool record_loss(const acquisition::SampleRecord& first,
                   const acquisition::SampleRecord& last,
                   uint64_t lost_count,
                   GapReason reason) noexcept {
    if (lost_count == 0U) return true;
    unlogged_records_ += lost_count;
    if (count_ == Capacity) {
      overflow_count_ += lost_count;
      return false;
    }
    GapRange range{};
    range.first_sequence = first.header.sequence;
    range.last_sequence = last.header.sequence;
    range.device_id = first.header.device_id;
    range.boot_id = first.header.boot_id;
    range.sensor = first.header.sensor;
    range.stream = first.header.stream;
    range.first_monotonic_us = first.header.monotonic_us;
    range.last_monotonic_us = last.header.monotonic_us;
    range.record_count = lost_count;
    range.reason = reason;
    range.present = true;
    ranges_[count_++] = range;
    return true;
  }
  uint64_t unlogged_records() const noexcept { return unlogged_records_; }
  uint64_t overflow_count() const noexcept { return overflow_count_; }
  std::size_t size() const noexcept { return count_; }
  const GapRange& at(std::size_t index) const noexcept { return ranges_[index]; }

 private:
  std::array<GapRange, Capacity> ranges_{};
  std::size_t count_{0U};
  uint64_t unlogged_records_{0U};
  uint64_t overflow_count_{0U};
};

class IArchiveStore {
 public:
  virtual ~IArchiveStore() = default;
  virtual AppendStatus append(const ImmutableRecordBytes& record) noexcept = 0;
  // Gap metadata is a reserved accounting path.  It must not consume the
  // pending-record quota, and a backend that cannot persist it reports a
  // retryable status so the runtime can expose the traceability limitation.
  virtual AppendStatus append_gap(const GapRange&) noexcept {
    return AppendStatus::Retryable;
  }
  virtual bool checkpoint() noexcept = 0;
  virtual bool recover() noexcept = 0;
  virtual uint64_t pending_bytes() const noexcept = 0;
  virtual uint64_t pending_quota_bytes() const noexcept = 0;
  virtual ArchiveHealth health() const noexcept = 0;
  // Optional media-migration seam. A store that owns pending records may
  // expose one record at a time and reclaim it only after a verified receipt.
  virtual bool peek_pending(ImmutableRecordBytes&) noexcept { return false; }
  // Optional read-only FIFO inspection seams. `index` is zero-based from the
  // current head. They must not reclaim, reorder or consume anything; stores
  // that cannot expose later entries keep the default (false). They exist so
  // bounded batch composition can look ahead without transferring ownership.
  // `peek_pending_identity_at` is the cheap form used on every main-loop poll;
  // `peek_pending_at` copies the record bytes and is only used when a body is
  // actually composed.
  virtual bool peek_pending_identity_at(std::size_t, ArchiveIdentity&) noexcept {
    return false;
  }
  virtual bool peek_pending_at(std::size_t, ImmutableRecordBytes&) noexcept {
    return false;
  }
  virtual bool reclaim_pending(const ArchiveIdentity&,
                               const config::ConfigHash&) noexcept {
    return false;
  }
  virtual bool contains(const ArchiveIdentity&,
                        const config::ConfigHash&) noexcept {
    return false;
  }
};

enum class StoreFault : uint8_t { None, ShortWrite, FlushFailure, CorruptTail, MediaFailure };

template <std::size_t Capacity>
class MemoryArchiveStore final : public IArchiveStore {
 public:
  static constexpr std::size_t kGapCapacity{32U};
  explicit MemoryArchiveStore(uint64_t quota_bytes) noexcept
      : quota_bytes_(quota_bytes) {}

  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
    if (!RecordCodec::valid(record)) return AppendStatus::Invalid;
    if (fault_ == StoreFault::MediaFailure || fault_ == StoreFault::ShortWrite) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    if (pending_bytes_ + record.size > quota_bytes_ || count_ == Capacity) {
      health_ = ArchiveHealth::Blocked;
      return AppendStatus::LoggingBlocked;
    }
    for (std::size_t i = 0U; i < count_; ++i) {
      if (same_identity(records_[i].identity, record.identity)) {
        return records_[i].hash == record.hash ? AppendStatus::Durable
                                               : AppendStatus::Invalid;
      }
    }
    records_[count_++] = record;
    pending_bytes_ += record.size;
    return AppendStatus::Durable;
  }

  AppendStatus append_gap(const GapRange& range) noexcept override {
    if (!range.present || range.record_count == 0U) {
      return AppendStatus::Invalid;
    }
    for (std::size_t i = 0U; i < gap_count_; ++i) {
      GapRange& existing = gaps_[i];
      if (existing.reason == range.reason &&
          existing.device_id.bytes == range.device_id.bytes &&
          existing.boot_id.bytes == range.boot_id.bytes &&
          existing.sensor == range.sensor && existing.stream == range.stream &&
          existing.first_sequence == range.first_sequence) {
        existing = range;
        return AppendStatus::Durable;
      }
    }
    if (gap_count_ == kGapCapacity || fault_ == StoreFault::MediaFailure ||
        fault_ == StoreFault::ShortWrite) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    gaps_[gap_count_++] = range;
    return AppendStatus::Durable;
  }

  bool checkpoint() noexcept override {
    if (fault_ == StoreFault::FlushFailure) return false;
    checkpointed_ = true;
    return true;
  }

  bool recover() noexcept override {
    if (fault_ == StoreFault::CorruptTail) {
      if (count_ != 0U) {
        records_[count_ - 1U].bytes[records_[count_ - 1U].size - 1U] ^= 0x01U;
      }
      fault_ = StoreFault::None;
    }
    while (count_ != 0U && !RecordCodec::valid(records_[count_ - 1U])) {
        pending_bytes_ -= records_[count_ - 1U].size;
        records_[count_ - 1U] = ImmutableRecordBytes{};
        --count_;
    }
    return true;
  }

  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t pending_quota_bytes() const noexcept override { return quota_bytes_; }
  ArchiveHealth health() const noexcept override { return health_; }
  bool peek_pending(ImmutableRecordBytes& output) noexcept override {
    if (count_ == 0U) return false;
    output = records_[0U];
    return true;
  }
  // Read-only lookahead for bounded batch composition. Copies into the
  // caller's scratch record; never reclaims or reorders the FIFO.
  bool peek_pending_at(std::size_t index,
                       ImmutableRecordBytes& output) noexcept override {
    if (index >= count_) return false;
    output = records_[index];
    return true;
  }
  // Cheap identity-only lookahead used on every main-loop poll.
  bool peek_pending_identity_at(std::size_t index,
                                ArchiveIdentity& output) noexcept override {
    if (index >= count_) return false;
    output = records_[index].identity;
    return true;
  }
  bool reclaim_pending(const ArchiveIdentity& identity,
                       const config::ConfigHash& hash) noexcept override {
    if (count_ == 0U || !same_identity(records_[0U].identity, identity) ||
        records_[0U].hash != hash) {
      return false;
    }
    pending_bytes_ -= records_[0U].size;
    for (std::size_t index = 1U; index < count_; ++index) {
      records_[index - 1U] = records_[index];
    }
    records_[count_ - 1U] = ImmutableRecordBytes{};
    --count_;
    return true;
  }
  bool contains(const ArchiveIdentity& identity,
                const config::ConfigHash& hash) noexcept override {
    for (std::size_t index = 0U; index < count_; ++index) {
      if (same_identity(records_[index].identity, identity)) {
        return records_[index].hash == hash;
      }
    }
    return false;
  }
  void set_fault(StoreFault fault) noexcept { fault_ = fault; }
  std::size_t size() const noexcept { return count_; }
  bool checkpointed() const noexcept { return checkpointed_; }
  const ImmutableRecordBytes& at(std::size_t index) const noexcept { return records_[index]; }
  std::size_t gap_size() const noexcept { return gap_count_; }
  const GapRange& gap_at(std::size_t index) const noexcept { return gaps_[index]; }
  void corrupt_last_byte() noexcept {
    if (count_ != 0U && records_[count_ - 1U].size != 0U) {
      records_[count_ - 1U].bytes[records_[count_ - 1U].size - 1U] ^= 0x01U;
    }
  }

 private:
  static bool same_identity(const ArchiveIdentity& left,
                            const ArchiveIdentity& right) noexcept {
    return left.kind == right.kind && left.sequence == right.sequence &&
           left.device_id.bytes == right.device_id.bytes &&
           left.boot_id.bytes == right.boot_id.bytes;
  }
  std::array<ImmutableRecordBytes, Capacity> records_{};
  std::array<GapRange, kGapCapacity> gaps_{};
  uint64_t quota_bytes_{0U};
  uint64_t pending_bytes_{0U};
  std::size_t count_{0U};
  std::size_t gap_count_{0U};
  StoreFault fault_{StoreFault::None};
  ArchiveHealth health_{ArchiveHealth::Healthy};
  bool checkpointed_{false};
};

class ArchiveAppendController final {
 public:
  explicit ArchiveAppendController(IArchiveStore& store) noexcept : store_(store) {}

  AppendStatus append(const ImmutableRecordBytes& record) noexcept {
    const auto status = store_.append(record);
    if (status == AppendStatus::Durable) {
      ++durable_records_;
    } else if (status == AppendStatus::Invalid) {
      gaps_.record(record.identity.sequence, GapReason::InvalidRecord);
    }
    // Retryable and LoggingBlocked remain pending. They become a terminal gap
    // only when the owning queue explicitly discards the identity.
    return status;
  }
  uint64_t durable_records() const noexcept { return durable_records_; }
  const GapTracker<32U>& gaps() const noexcept { return gaps_; }
  ArchiveHealth health() const noexcept { return store_.health(); }

 private:
  IArchiveStore& store_;
  GapTracker<32U> gaps_{};
  uint64_t durable_records_{0U};
};

}  // namespace sigap::storage
