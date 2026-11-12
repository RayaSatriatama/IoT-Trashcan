#include "debug/service.h"

#include <cstring>
#include <limits>

#include "debug/registry.h"

namespace sigap::debug {
namespace {
bool valid_level(Level level) noexcept {
  return static_cast<uint8_t>(level) <= static_cast<uint8_t>(Level::Debug);
}
bool valid_build(std::string_view build) noexcept {
  if (build.empty() || build.size() > 96U) return false;
  for (const unsigned char byte : build) {
    if (byte < 0x20U || byte > 0x7eU) return false;
  }
  return true;
}
}  // namespace

DebugService::DebugService(const acquisition::BootId& boot,
                           std::string_view build, Level initial_level,
                           IDebugLock& lock) noexcept
    : DebugService(boot, build, initial_level, lock,
                   acquisition::SequenceAllocator{1U}) {}

DebugService::DebugService(const acquisition::BootId& boot,
                           std::string_view build, Level initial_level,
                           IDebugLock& lock,
                           acquisition::SequenceAllocator sequence) noexcept
    : boot_(boot), level_(initial_level), lock_(lock), sequence_(sequence) {
  identity_valid_ = boot.present && valid_build(build) && valid_level(initial_level);
  if (identity_valid_) std::memcpy(build_id_.data(), build.data(), build.size());
}

bool DebugService::valid_identity() const noexcept {
  if (!identity_valid_ || !valid_level(level_)) return false;
  return sequence_.next_value() != 0U;
}

bool DebugService::valid_event(const EventInput& input) const noexcept {
  constexpr uint8_t allowed = kHasRequestId | kHasRelatedSequence;
  return valid_level(input.level) && registered(input.module, input.event_code) &&
         (input.presence & static_cast<uint8_t>(~allowed)) == 0U &&
         ((input.presence & kHasRequestId) != 0U || input.request_id == 0U) &&
         ((input.presence & kHasRelatedSequence) != 0U || input.related_sequence == 0U);
}

bool DebugService::emit(const EventInput& input) noexcept {
  lock_.lock();
  if (!valid_identity() || !valid_event(input) ||
      static_cast<uint8_t>(input.level) > static_cast<uint8_t>(level_)) {
    lock_.unlock();
    return false;
  }
  uint64_t sequence = 0U;
  if (!sequence_.allocate(sequence)) {
    lock_.unlock();
    return false;
  }
  EventRecord record{};
  record.sequence = sequence;
  record.monotonic_us = input.monotonic_us;
  record.related_sequence = input.related_sequence;
  record.request_id = input.request_id;
  record.duration_us = input.duration_us;
  record.arg0 = input.arg0;
  record.arg1 = input.arg1;
  record.event_code = input.event_code;
  record.level = input.level;
  record.module = input.module;
  record.presence = input.presence;
  if (count_ == records_.size()) {
    records_[head_] = record;
    head_ = (head_ + 1U) % records_.size();
    saturating_add(overwritten_, 1U);
  } else {
    records_[(head_ + count_) % records_.size()] = record;
    ++count_;
  }
  lock_.unlock();
  return true;
}

Meta DebugService::meta_locked(uint64_t now_us) const noexcept {
  Meta result{};
  result.boot = boot_;
  result.build_id = build_id_;
  result.level = level_;
  result.oldest = count_ == 0U ? 0U : records_[head_].sequence;
  result.latest = count_ == 0U ? 0U : records_[(head_ + count_ - 1U) % records_.size()].sequence;
  result.overwritten = overwritten_;
  result.delivery_lost = delivery_lost_;
  result.usb_lost = usb_lost_;
  result.captured_monotonic_us = now_us;
  result.sequence_exhausted = sequence_.exhausted();
  return result;
}

Meta DebugService::meta(uint64_t now_us) const noexcept {
  lock_.lock();
  const Meta result = meta_locked(now_us);
  lock_.unlock();
  return result;
}

Page DebugService::read(const Cursor& cursor) const noexcept {
  lock_.lock();
  Page result{};
  result.boot = boot_;
  result.level = level_;
  result.next_after = cursor.after;
  result.oldest = count_ == 0U ? 0U : records_[head_].sequence;
  result.overwritten = overwritten_;
  if (!cursor.boot.present || !boot_.present || cursor.boot.bytes != boot_.bytes) {
    result.boot_changed = true;
    result.next_after = 0U;
  } else if (count_ != 0U) {
    result.gap = cursor.after < result.oldest - 1U;
    for (size_t i = 0U; i < count_; ++i) {
      const EventRecord& record = records_[(head_ + i) % records_.size()];
      if (record.sequence > cursor.after) {
        result.has_event = true;
        result.event = record;
        result.next_after = record.sequence;
        break;
      }
    }
  }
  lock_.unlock();
  return result;
}

bool DebugService::capture(const Cursor& cursor, size_t max_events,
                           uint64_t now_us, BatchSnapshot& out) const noexcept {
  out = BatchSnapshot{};
  if (max_events == 0U || max_events > out.events.size()) return false;
  lock_.lock();
  if (!valid_identity()) {
    lock_.unlock();
    return false;
  }
  out.meta = meta_locked(now_us);
  if (!cursor.boot.present || !boot_.present || cursor.boot.bytes != boot_.bytes) {
    out.boot_changed = true;
  } else if (count_ != 0U) {
    out.gap = cursor.after < out.meta.oldest - 1U;
    for (size_t i = 0U; i < count_ && out.count < max_events; ++i) {
      const EventRecord& record = records_[(head_ + i) % records_.size()];
      if (record.sequence <= cursor.after) continue;
      if (record.monotonic_us > now_us) {
        out = BatchSnapshot{};
        lock_.unlock();
        return false;
      }
      out.events[out.count++] = record;
    }
  }
  lock_.unlock();
  return true;
}

bool DebugService::set_level(Level level) noexcept {
  if (!valid_level(level)) return false;
  lock_.lock();
  if (!valid_identity()) {
    lock_.unlock();
    return false;
  }
  level_ = level;
  lock_.unlock();
  return true;
}

void DebugService::saturating_add(uint64_t& value, uint64_t count) noexcept {
  value = count > std::numeric_limits<uint64_t>::max() - value
              ? std::numeric_limits<uint64_t>::max()
              : value + count;
}

void DebugService::note_delivery_loss(uint64_t count) noexcept {
  lock_.lock();
  saturating_add(delivery_lost_, count);
  lock_.unlock();
}

void DebugService::note_usb_loss(uint64_t count) noexcept {
  lock_.lock();
  saturating_add(usb_lost_, count);
  lock_.unlock();
}

}  // namespace sigap::debug
