#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "acquisition/acquisition_types.h"

namespace sigap::processing {

inline constexpr std::size_t kMaxProfileRefsPerResult{16U};

enum class CollectionStatus : uint8_t {
  Added,
  Full,
  Invalid,
  Duplicate,
  Conflict,
};

enum class InputQuality : uint8_t {
  Ready,
  Missing,
  Timeout,
  Stale,
  Uncalibrated,
  Calibrating,
  OutOfRange,
  Disconnected,
  CrcError,
};

enum class ProcessingReason : uint8_t {
  Ready,
  InputMissing,
  InputTimeout,
  InputStale,
  InputUncalibrated,
  InputCalibrating,
  InputOutOfRange,
  InputDisconnected,
  InputCrcError,
  ProfileMissing,
  ProfileInvalid,
  CapacityMissing,
  CapacityInvalid,
  ConversionFailed,
  SourceCapacityExceeded,
};

struct InputDisposition final {
  InputQuality quality{InputQuality::Missing};
  ProcessingReason reason{ProcessingReason::InputMissing};

  constexpr bool valid() const noexcept {
    return (quality == InputQuality::Ready) ==
           (reason == ProcessingReason::Ready);
  }

  constexpr bool operational() const noexcept {
    return valid() && quality == InputQuality::Ready;
  }
};

struct SourceSampleId final {
  acquisition::DeviceId device_id{};
  acquisition::BootId boot_id{};
  uint64_t sequence{0U};

  constexpr bool valid() const noexcept {
    return device_id.present && boot_id.present;
  }
};

inline constexpr bool same_source_sample_id(
    const SourceSampleId& left,
    const SourceSampleId& right) noexcept {
  return left.sequence == right.sequence &&
         left.device_id.present == right.device_id.present &&
         left.device_id.bytes == right.device_id.bytes &&
         left.boot_id.present == right.boot_id.present &&
         left.boot_id.bytes == right.boot_id.bytes;
}

template <std::size_t Capacity>
class SourceSampleIds final {
  static_assert(Capacity > 0U,
                "A processing result must allow at least one source ID");

 public:
  CollectionStatus try_add(const SourceSampleId& value) noexcept {
    if (!value.valid()) {
      return CollectionStatus::Invalid;
    }
    for (std::size_t index = 0U; index < size_; ++index) {
      if (same_source_sample_id(values_[index], value)) {
        return CollectionStatus::Duplicate;
      }
    }
    if (size_ == Capacity) {
      overflowed_ = true;
      return CollectionStatus::Full;
    }
    values_[size_] = value;
    ++size_;
    return CollectionStatus::Added;
  }

  constexpr std::size_t size() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0U; }
  constexpr bool overflowed() const noexcept { return overflowed_; }
  static constexpr std::size_t capacity() noexcept { return Capacity; }

  const SourceSampleId* at(std::size_t index) const noexcept {
    return index < size_ ? &values_[index] : nullptr;
  }

 private:
  std::array<SourceSampleId, Capacity> values_{};
  std::size_t size_{0U};
  bool overflowed_{false};
};

inline bool same_profile_key(const acquisition::ProfileRef& left,
                             const acquisition::ProfileRef& right) noexcept {
  return left.kind == right.kind && left.target_id == right.target_id;
}

class ProfileRefSet final {
 public:
  CollectionStatus try_add(const acquisition::ProfileRef& value) noexcept {
    if (!value.present || value.schema_version == 0U) {
      return CollectionStatus::Invalid;
    }
    for (std::size_t index = 0U; index < size_; ++index) {
      if (same_profile_key(values_[index], value)) {
        const auto& existing = values_[index];
        const bool same_identity =
            existing.schema_version == value.schema_version &&
            existing.profile_version == value.profile_version &&
            existing.hash == value.hash;
        return same_identity ? CollectionStatus::Duplicate
                             : CollectionStatus::Conflict;
      }
    }
    if (size_ == values_.size()) {
      overflowed_ = true;
      return CollectionStatus::Full;
    }
    values_[size_] = value;
    ++size_;
    return CollectionStatus::Added;
  }

  constexpr std::size_t size() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0U; }
  constexpr bool overflowed() const noexcept { return overflowed_; }
  static constexpr std::size_t capacity() noexcept {
    return kMaxProfileRefsPerResult;
  }

  const acquisition::ProfileRef* at(std::size_t index) const noexcept {
    return index < size_ ? &values_[index] : nullptr;
  }

 private:
  std::array<acquisition::ProfileRef, kMaxProfileRefsPerResult> values_{};
  std::size_t size_{0U};
  bool overflowed_{false};
};

enum class SnapshotStatus : uint8_t {
  Captured,
  InvalidConfig,
  NoSources,
  SourceOverflow,
  ProfileOverflow,
};

template <std::size_t SourceCapacity>
class CycleSnapshot final {
 public:
  static SnapshotStatus capture(
      uint64_t started_monotonic_us,
      const acquisition::ConfigRef& config_ref,
      const ProfileRefSet& profile_refs,
      const SourceSampleIds<SourceCapacity>& source_sample_ids,
      CycleSnapshot& output) noexcept {
    if (!config_ref.present || config_ref.schema_version == 0U) {
      return SnapshotStatus::InvalidConfig;
    }
    if (source_sample_ids.empty()) {
      return SnapshotStatus::NoSources;
    }
    if (source_sample_ids.overflowed()) {
      return SnapshotStatus::SourceOverflow;
    }
    if (profile_refs.overflowed()) {
      return SnapshotStatus::ProfileOverflow;
    }

    CycleSnapshot candidate{};
    candidate.started_monotonic_us_ = started_monotonic_us;
    candidate.config_ref_ = config_ref;
    candidate.profile_refs_ = profile_refs;
    candidate.source_sample_ids_ = source_sample_ids;
    candidate.present_ = true;
    output = candidate;
    return SnapshotStatus::Captured;
  }

  constexpr bool present() const noexcept { return present_; }
  constexpr uint64_t started_monotonic_us() const noexcept {
    return started_monotonic_us_;
  }
  constexpr const acquisition::ConfigRef& config_ref() const noexcept {
    return config_ref_;
  }
  constexpr const ProfileRefSet& profile_refs() const noexcept {
    return profile_refs_;
  }
  constexpr const SourceSampleIds<SourceCapacity>& source_sample_ids()
      const noexcept {
    return source_sample_ids_;
  }

 private:
  bool present_{false};
  uint64_t started_monotonic_us_{0U};
  acquisition::ConfigRef config_ref_{};
  ProfileRefSet profile_refs_{};
  SourceSampleIds<SourceCapacity> source_sample_ids_{};
};

// FIS and Safety expose distinct adapter slots. Their concrete result types,
// including their state enums, remain owned by their respective modules.
template <typename Value, std::size_t MaxBytes>
class FuzzyResultAdapterSlot final {
  static_assert(std::is_trivially_copyable<Value>::value,
                "FIS adapter values must have bounded value semantics");
  static_assert(sizeof(Value) <= MaxBytes,
                "FIS adapter exceeds its declared bound");

 public:
  explicit constexpr FuzzyResultAdapterSlot(const Value& value) noexcept
      : value_(value) {}

  static constexpr std::size_t capacity_bytes() noexcept { return MaxBytes; }
  constexpr const Value& value() const noexcept { return value_; }

 private:
  Value value_{};
};

template <typename Value, std::size_t MaxBytes>
class SafetyResultAdapterSlot final {
  static_assert(std::is_trivially_copyable<Value>::value,
                "Safety adapter values must have bounded value semantics");
  static_assert(sizeof(Value) <= MaxBytes,
                "Safety adapter exceeds its declared bound");

 public:
  explicit constexpr SafetyResultAdapterSlot(const Value& value) noexcept
      : value_(value) {}

  static constexpr std::size_t capacity_bytes() noexcept { return MaxBytes; }
  constexpr const Value& value() const noexcept { return value_; }

 private:
  Value value_{};
};

}  // namespace sigap::processing
