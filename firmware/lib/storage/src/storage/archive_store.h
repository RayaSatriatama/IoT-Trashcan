#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "archive_codec.h"
#include "config/config_types.h"

namespace sigap::storage {

enum class AppendStatus : uint8_t { Durable, Retryable, LoggingBlocked, Invalid };
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
  virtual bool checkpoint() noexcept = 0;
  virtual bool recover() noexcept = 0;
  virtual uint64_t pending_bytes() const noexcept = 0;
  virtual uint64_t pending_quota_bytes() const noexcept = 0;
  virtual ArchiveHealth health() const noexcept = 0;
};

enum class StoreFault : uint8_t { None, ShortWrite, FlushFailure, CorruptTail, MediaFailure };

template <std::size_t Capacity>
class MemoryArchiveStore final : public IArchiveStore {
 public:
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
  void set_fault(StoreFault fault) noexcept { fault_ = fault; }
  std::size_t size() const noexcept { return count_; }
  bool checkpointed() const noexcept { return checkpointed_; }
  const ImmutableRecordBytes& at(std::size_t index) const noexcept { return records_[index]; }
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
  uint64_t quota_bytes_{0U};
  uint64_t pending_bytes_{0U};
  std::size_t count_{0U};
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
