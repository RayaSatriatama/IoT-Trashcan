#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "debug/service.h"
namespace sigap::debug {
class IUsbDebugWriter {
 public:
  virtual ~IUsbDebugWriter() = default;
  virtual size_t writable() noexcept = 0;
  virtual size_t write(const char*, size_t) noexcept = 0;
};
class UsbDebugSink final {
 public:
  UsbDebugSink(DebugService& service, IUsbDebugWriter& writer) noexcept;
  void poll(uint64_t now_us, size_t byte_budget) noexcept;
 private:
  DebugService& service_;
  IUsbDebugWriter& writer_;
  Cursor cursor_{};
  std::array<char, 384U> pending_{};
  size_t pending_size_{0U};
  size_t pending_offset_{0U};
  uint64_t pending_sequence_{0U};
  bool cursor_initialized_{false};
  bool pending_event_{false};
};
static_assert(sizeof(UsbDebugSink) <= 512U, "USB debug sink exceeds bounded state");
}
