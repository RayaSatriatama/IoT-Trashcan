#pragma once
#include <cstdint>
namespace sigap::debug {
enum class IoOwner : uint8_t { ArchiveHttp, ControlHttp, DebugHttp, Filesystem };
class RuntimeIoGate final {
 public:
  bool try_acquire(IoOwner owner) noexcept {
    const uint8_t bit = mask(owner);
    if (bit == 0U || (owners_ & bit) != 0U) return false;
    constexpr uint8_t http = 0x07U;
    if (owner == IoOwner::Filesystem) {
      if ((owners_ & http) != 0U) return false;
    } else if (owner == IoOwner::DebugHttp) {
      if (owners_ != 0U) return false;
    } else if ((owners_ & (mask(IoOwner::DebugHttp) | mask(IoOwner::Filesystem))) != 0U) {
      return false;
    }
    owners_ = static_cast<uint8_t>(owners_ | bit);
    return true;
  }
  void release(IoOwner owner) noexcept { owners_ = static_cast<uint8_t>(owners_ & ~mask(owner)); }
  bool active(IoOwner owner) const noexcept { return (owners_ & mask(owner)) != 0U; }
 private:
  static constexpr uint8_t mask(IoOwner owner) noexcept {
    return static_cast<uint8_t>(1U << static_cast<uint8_t>(owner));
  }
  uint8_t owners_{};
};
} // namespace sigap::debug
