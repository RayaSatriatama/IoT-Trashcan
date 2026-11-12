#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "acquisition/sequence.h"
#include "debug/types.h"

namespace sigap::debug {

class IDebugLock {
 public:
  virtual ~IDebugLock() = default;
  virtual void lock() noexcept = 0;
  virtual void unlock() noexcept = 0;
};

class DebugService final : public IEventSink {
 public:
  DebugService(const acquisition::BootId& boot, std::string_view build,
               Level initial_level, IDebugLock& lock) noexcept;
  DebugService(const acquisition::BootId& boot, std::string_view build,
               Level initial_level, IDebugLock& lock,
               acquisition::SequenceAllocator sequence) noexcept;

  bool emit(const EventInput& input) noexcept override;
  Meta meta(uint64_t now_us) const noexcept;
  Page read(const Cursor& cursor) const noexcept;
  bool capture(const Cursor& cursor, size_t max_events, uint64_t now_us,
               BatchSnapshot& out) const noexcept;
  bool set_level(Level level) noexcept;
  void note_delivery_loss(uint64_t count) noexcept;
  void note_usb_loss(uint64_t count) noexcept;

 private:
  Meta meta_locked(uint64_t now_us) const noexcept;
  bool valid_identity() const noexcept;
  bool valid_event(const EventInput& input) const noexcept;
  void saturating_add(uint64_t& value, uint64_t count) noexcept;

  acquisition::BootId boot_{};
  std::array<char, 97> build_id_{};
  Level level_{Level::Info};
  IDebugLock& lock_;
  acquisition::SequenceAllocator sequence_{};
  bool identity_valid_{};
  std::array<EventRecord, 64> records_{};
  size_t head_{};
  size_t count_{};
  uint64_t overwritten_{};
  uint64_t delivery_lost_{};
  uint64_t usb_lost_{};
};

static_assert(sizeof(EventRecord) * 64U <= 4096U,
              "debug event ring exceeds RAM contract");

}  // namespace sigap::debug
