#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace sigap::provisioning {

enum class BleServiceEventKind : uint8_t { Connect, Write, Disconnect };

struct BleServiceConnection final {
  uint64_t epoch{};
  uint16_t handle{0xFFFFU};
  bool connected{};
  bool failed{};
};

struct BleServiceEvent final {
  uint64_t epoch{};
  uint64_t received_us{};
  uint16_t handle{0xFFFFU};
  uint16_t size{};
  BleServiceEventKind kind{BleServiceEventKind::Write};
  std::array<char, 192> payload{};

  ~BleServiceEvent() { clear(); }
  void clear() noexcept {
    volatile char* bytes = payload.data();
    for (size_t i = 0; i < payload.size(); ++i) bytes[i] = 0;
    size = 0;
    epoch = received_us = 0;
    handle = 0xFFFFU;
    kind = BleServiceEventKind::Write;
  }
};

// The service holds its short queue lock for every access, never for dispatch/notify.
class BleServiceQueue final {
 public:
  static constexpr size_t kCapacity{16};

  void connect(uint16_t handle, uint64_t now_us) noexcept {
    clear();
    handle_ = handle;
    connected_ = true;
    terminated_ = failed_ = failure_pending_ = false;
    if (epoch_ == std::numeric_limits<uint64_t>::max()) {
      fail();
      return;
    }
    ++epoch_;
    push(BleServiceEventKind::Connect, {}, now_us);
  }

  bool write(uint16_t handle, std::string_view bytes, uint64_t now_us) noexcept {
    if (!connected_ || failed_ || handle != handle_) return false;
    if (bytes.size() > 192 || count_ == kCapacity) {
      fail();
      return false;
    }
    push(BleServiceEventKind::Write, bytes, now_us);
    return true;
  }

  bool disconnect(uint16_t handle, uint64_t now_us) noexcept {
    if (!connected_ || handle != handle_) return false;
    connected_ = false;
    failure_pending_ = false;
    clear();
    push(BleServiceEventKind::Disconnect, {}, now_us);
    return true;
  }

  bool pop(BleServiceEvent& event) noexcept {
    event.clear();
    if (count_ == 0) return false;
    event = events_[head_];
    events_[head_].clear();
    head_ = (head_ + 1) % kCapacity;
    --count_;
    return true;
  }

  BleServiceConnection connection() const noexcept {
    return {epoch_, handle_, connected_, failed_};
  }

  bool matches(const BleServiceEvent& event) const noexcept {
    if (event.epoch != epoch_ || event.handle != handle_) return false;
    return event.kind == BleServiceEventKind::Disconnect
        ? !connected_ : connected_ && !failed_;
  }

  bool consume_failure() noexcept {
    const bool pending = failure_pending_;
    failure_pending_ = false;
    return pending;
  }

  bool terminate(const BleServiceConnection& expected) noexcept {
    if (expected.epoch != epoch_ || expected.handle != handle_ ||
        !connected_ || terminated_) return false;
    terminated_ = failed_ = true;
    failure_pending_ = false;
    clear();
    return true;
  }

 private:
  void clear() noexcept {
    for (auto& event : events_) event.clear();
    head_ = count_ = 0;
  }

  void fail() noexcept {
    failed_ = failure_pending_ = true;
    clear();
  }

  void push(BleServiceEventKind kind, std::string_view bytes,
            uint64_t now_us) noexcept {
    auto& event = events_[(head_ + count_) % kCapacity];
    event.kind = kind;
    event.epoch = epoch_;
    event.handle = handle_;
    event.received_us = now_us;
    event.size = static_cast<uint16_t>(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) event.payload[i] = bytes[i];
    ++count_;
  }

  std::array<BleServiceEvent, kCapacity> events_{};
  size_t head_{};
  size_t count_{};
  uint64_t epoch_{};
  uint16_t handle_{0xFFFFU};
  bool connected_{};
  bool failed_{};
  bool failure_pending_{};
  bool terminated_{};
};

}  // namespace sigap::provisioning
