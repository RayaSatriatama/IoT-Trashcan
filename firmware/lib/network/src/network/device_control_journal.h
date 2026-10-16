#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "network/device_control.h"

#ifdef ARDUINO
#include <FS.h>
#endif

namespace sigap::network {

// The journal is deliberately a bounded, two-slot record.  A new record is
// published to the inactive slot and validated before it becomes eligible for
// recovery.  The previous slot is never overwritten by a partial write.
inline constexpr uint32_t kDeviceControlJournalMagic{0x53494752U};
inline constexpr uint16_t kDeviceControlJournalVersion{1U};
inline constexpr std::size_t kDeviceControlJournalFrameBytes{12U * 1024U};
inline constexpr std::size_t kDeviceControlJournalPathBytes{112U};

enum class DeviceControlJournalState : uint8_t {
  Empty,
  Valid,
  Corrupt,
  IoError,
};

namespace detail {

class JournalWriter final {
 public:
  JournalWriter(uint8_t* bytes, std::size_t capacity) noexcept
      : bytes_(bytes), capacity_(capacity) {}

  bool u8(uint8_t value) noexcept { return put(&value, sizeof(value)); }
  bool u16(uint16_t value) noexcept { return little(value); }
  bool u32(uint32_t value) noexcept { return little(value); }
  bool u64(uint64_t value) noexcept { return little(value); }
  bool raw(const void* data, std::size_t size) noexcept {
    if (data == nullptr || size > capacity_ - position_) return false;
    std::memcpy(bytes_ + position_, data, size);
    position_ += size;
    return true;
  }
  template <std::size_t N>
  bool text(const DeviceControlText<N>& value) noexcept {
    if (value.size > UINT16_MAX || !u16(static_cast<uint16_t>(value.size))) return false;
    return raw(value.bytes.data(), value.size);
  }
  std::size_t size() const noexcept { return position_; }

 private:
  template <typename T>
  bool little(T value) noexcept {
    std::array<uint8_t, sizeof(T)> encoded{};
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
      encoded[i] = static_cast<uint8_t>(value >> (i * 8U));
    }
    return raw(encoded.data(), encoded.size());
  }
  bool put(const void* data, std::size_t size) noexcept { return raw(data, size); }
  uint8_t* bytes_{nullptr};
  std::size_t capacity_{0U};
  std::size_t position_{0U};
};

class JournalReader final {
 public:
  JournalReader(const uint8_t* bytes, std::size_t size) noexcept
      : bytes_(bytes), size_(size) {}

  bool u8(uint8_t& value) noexcept { return take(&value, sizeof(value)); }
  bool u16(uint16_t& value) noexcept { return little(value); }
  bool u32(uint32_t& value) noexcept { return little(value); }
  bool u64(uint64_t& value) noexcept { return little(value); }
  bool raw(void* data, std::size_t size) noexcept {
    if (data == nullptr || size > size_ - position_) return false;
    std::memcpy(data, bytes_ + position_, size);
    position_ += size;
    return true;
  }
  template <std::size_t N>
  bool text(DeviceControlText<N>& value, bool allow_empty = false) noexcept {
    uint16_t count{0U};
    if (!u16(count) || (!allow_empty && count == 0U) || count > N) return false;
    if (allow_empty && count == 0U) {
      value = DeviceControlText<N>{};
      return true;
    }
    std::array<char, N> decoded{};
    if (!raw(decoded.data(), count) || !value.assign(std::string_view(decoded.data(), count))) {
      return false;
    }
    return true;
  }
  std::size_t remaining() const noexcept { return size_ - position_; }

 private:
  template <typename T>
  bool little(T& value) noexcept {
    std::array<uint8_t, sizeof(T)> encoded{};
    if (!raw(encoded.data(), encoded.size())) return false;
    value = 0;
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
      value |= static_cast<T>(encoded[i]) << (i * 8U);
    }
    return true;
  }
  bool take(void* data, std::size_t size) noexcept { return raw(data, size); }
  const uint8_t* bytes_{nullptr};
  std::size_t size_{0U};
  std::size_t position_{0U};
};

inline uint32_t crc32(const uint8_t* bytes, std::size_t size) noexcept {
  uint32_t crc{0xFFFFFFFFU};
  for (std::size_t i = 0U; i < size; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

inline bool encode(const DeviceControlCommand& command,
                   const DeviceControlResult& result, uint64_t generation,
                   uint8_t* frame, std::size_t capacity,
                   std::size_t& frame_size) noexcept {
  if (frame == nullptr || capacity < 32U || command.payload_size > command.payload.size() ||
      result.payload_size > result.payload.size()) return false;
  JournalWriter writer(frame, capacity);
  if (!writer.u32(kDeviceControlJournalMagic) ||
      !writer.u16(kDeviceControlJournalVersion) || !writer.u16(0U) ||
      !writer.u64(generation) || !writer.u32(0U) ||
      !writer.text(command.command_id) || !writer.text(command.device_id) ||
      !writer.text(command.operator_id) || !writer.text(command.session_id) ||
      !writer.text(command.sensor_id) || !writer.text(command.operation) ||
      !writer.text(command.request_hash) || !writer.u64(command.expires_at_us) ||
      !writer.u32(command.attempt) ||
      !writer.u16(static_cast<uint16_t>(command.payload_size)) ||
      !writer.raw(command.payload.data(), command.payload_size) ||
      !writer.u8(static_cast<uint8_t>(result.status)) ||
      !writer.text(result.command_id) || !writer.text(result.session_id) ||
      !writer.text(result.error_code) ||
      !writer.u16(static_cast<uint16_t>(result.payload_size)) ||
      !writer.raw(result.payload.data(), result.payload_size)) return false;
  const std::size_t body_end = writer.size();
  if (body_end + sizeof(uint32_t) > capacity) return false;
  const std::size_t total_size = body_end + sizeof(uint32_t);
  frame[16U] = static_cast<uint8_t>(total_size & 0xFFU);
  frame[17U] = static_cast<uint8_t>((total_size >> 8U) & 0xFFU);
  frame[18U] = static_cast<uint8_t>((total_size >> 16U) & 0xFFU);
  frame[19U] = static_cast<uint8_t>((total_size >> 24U) & 0xFFU);
  const uint32_t checksum = crc32(frame, body_end);
  frame[body_end] = static_cast<uint8_t>(checksum & 0xFFU);
  frame[body_end + 1U] = static_cast<uint8_t>((checksum >> 8U) & 0xFFU);
  frame[body_end + 2U] = static_cast<uint8_t>((checksum >> 16U) & 0xFFU);
  frame[body_end + 3U] = static_cast<uint8_t>((checksum >> 24U) & 0xFFU);
  frame_size = total_size;
  return true;
}

inline bool frame_info(const uint8_t* frame, std::size_t frame_size,
                       uint64_t& generation) noexcept {
  if (frame == nullptr || frame_size < 24U ||
      frame_size > kDeviceControlJournalFrameBytes) return false;
  JournalReader reader(frame, frame_size);
  uint32_t magic{0U}, total{0U};
  uint16_t version{0U}, reserved{0U};
  if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(reserved) ||
      !reader.u64(generation) || !reader.u32(total) ||
      magic != kDeviceControlJournalMagic || version != kDeviceControlJournalVersion ||
      total != frame_size || frame_size < sizeof(uint32_t) ||
      reader.remaining() < sizeof(uint32_t)) return false;
  const std::size_t checksum_offset = frame_size - sizeof(uint32_t);
  uint32_t expected{0U};
  expected = static_cast<uint32_t>(frame[checksum_offset]) |
             (static_cast<uint32_t>(frame[checksum_offset + 1U]) << 8U) |
             (static_cast<uint32_t>(frame[checksum_offset + 2U]) << 16U) |
             (static_cast<uint32_t>(frame[checksum_offset + 3U]) << 24U);
  return crc32(frame, checksum_offset) == expected;
}

inline bool decode(const uint8_t* frame, std::size_t frame_size,
                   DeviceControlCommand& command,
                   DeviceControlResult& result) noexcept {
  uint64_t generation{0U};
  if (!frame_info(frame, frame_size, generation)) return false;
  JournalReader reader(frame, frame_size - sizeof(uint32_t));
  uint32_t magic{0U}, total{0U};
  uint16_t version{0U}, reserved{0U};
  if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(reserved) ||
      !reader.u64(generation) || !reader.u32(total) ||
      !reader.text(command.command_id) || !reader.text(command.device_id) ||
      !reader.text(command.operator_id) || !reader.text(command.session_id) ||
      !reader.text(command.sensor_id) || !reader.text(command.operation) ||
      !reader.text(command.request_hash) || !reader.u64(command.expires_at_us) ||
      !reader.u32(command.attempt)) return false;
  uint16_t payload_size{0U};
  if (!reader.u16(payload_size) || payload_size > command.payload.size() ||
      !reader.raw(command.payload.data(), payload_size)) return false;
  uint8_t status{0U};
  if (!reader.u8(status) || status > static_cast<uint8_t>(DeviceControlStatus::Cancelled) ||
      !reader.text(result.command_id) || !reader.text(result.session_id) ||
      !reader.text(result.error_code, true)) return false;
  uint16_t result_size{0U};
  if (!reader.u16(result_size) || result_size > result.payload.size() ||
      !reader.raw(result.payload.data(), result_size) || reader.remaining() != 0U) return false;
  command.payload_size = payload_size;
  result.status = static_cast<DeviceControlStatus>(status);
  result.payload_size = result_size;
  return true;
}

}  // namespace detail

// Native implementation uses two ordinary files, allowing tests to exercise
// persistence across object recreation without touching target media.
#ifndef ARDUINO
class NativeDeviceControlResultJournal final : public IDeviceControlResultJournal {
 public:
  explicit NativeDeviceControlResultJournal(const char* path) noexcept { set_path(path); }

  DeviceControlJournalState state() const noexcept { return state_; }
  bool recover() noexcept override {
    SlotInfo selected{};
    return scan(selected) || state_ == DeviceControlJournalState::Empty;
  }
  bool load(DeviceControlCommand& command, DeviceControlResult& result) const noexcept override {
    SlotInfo selected{};
    if (!scan(selected)) return false;
    if (!selected.valid) return false;
    if (!read_file(selected.path.data(), frame_.data(), frame_size_)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    if (!detail::decode(frame_.data(), frame_size_, command, result)) {
      state_ = DeviceControlJournalState::Corrupt;
      return false;
    }
    state_ = DeviceControlJournalState::Valid;
    return true;
  }

  bool save(const DeviceControlCommand& command, const DeviceControlResult& result) noexcept override {
    SlotInfo selected{};
    (void)scan(selected);
    if (!selected.valid && state_ != DeviceControlJournalState::Empty) return false;
    const uint64_t generation = selected.valid ? selected.generation + 1U : 1U;
    std::size_t encoded{0U};
    if (!detail::encode(command, result, generation, frame_.data(), frame_.size(), encoded)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    std::array<char, kDeviceControlJournalPathBytes> target{};
    if (!slot_path(selected.valid && selected.suffix == 'a' ? 'b' : 'a', target)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    if (!write_file(target.data(), frame_.data(), encoded) ||
        !read_file(target.data(), frame_.data(), frame_size_)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    uint64_t verified_generation{0U};
    if (!detail::frame_info(frame_.data(), frame_size_, verified_generation) ||
        verified_generation != generation) {
      state_ = DeviceControlJournalState::Corrupt;
      return false;
    }
    state_ = DeviceControlJournalState::Valid;
    return true;
  }

  bool clear() noexcept override {
    bool ok = remove_if_present('a') && remove_if_present('b');
    state_ = ok ? DeviceControlJournalState::Empty : DeviceControlJournalState::IoError;
    return ok;
  }

 private:
  struct SlotInfo final {
    bool valid{false};
    bool io_error{false};
    char suffix{0};
    uint64_t generation{0U};
    std::array<char, kDeviceControlJournalPathBytes> path{};
  };

  void set_path(const char* path) noexcept {
    if (path == nullptr) return;
    std::strncpy(path_.data(), path, path_.size() - 3U);
    path_[path_.size() - 3U] = '\0';
  }
  bool slot_path(char suffix, std::array<char, kDeviceControlJournalPathBytes>& out) const noexcept {
    const int written = std::snprintf(out.data(), out.size(), "%s.%c", path_.data(), suffix);
    return written > 0 && static_cast<std::size_t>(written) < out.size();
  }
  bool read_file(const char* path, uint8_t* output, std::size_t& size) const noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    if (std::fseek(file, 0L, SEEK_END) != 0) { std::fclose(file); return false; }
    const long length = std::ftell(file);
    if (length <= 0L || static_cast<std::size_t>(length) > kDeviceControlJournalFrameBytes ||
        std::fseek(file, 0L, SEEK_SET) != 0) { std::fclose(file); return false; }
    size = static_cast<std::size_t>(length);
    const bool ok = std::fread(output, 1U, size, file) == size;
    std::fclose(file);
    return ok;
  }
  bool write_file(const char* path, const uint8_t* data, std::size_t size) const noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) return false;
    const bool ok = std::fwrite(data, 1U, size, file) == size && std::fflush(file) == 0;
    const int close_result = std::fclose(file);
    return ok && close_result == 0;
  }
  bool remove_if_present(char suffix) const noexcept {
    std::array<char, kDeviceControlJournalPathBytes> path{};
    if (!slot_path(suffix, path)) return false;
    if (std::remove(path.data()) == 0) return true;
    std::FILE* file = std::fopen(path.data(), "rb");
    if (file == nullptr) return true;
    std::fclose(file);
    return false;
  }
  bool scan(SlotInfo& selected) const noexcept {
    selected = SlotInfo{};
    bool saw_corrupt{false};
    bool saw_io{false};
    for (const char suffix : {'a', 'b'}) {
      SlotInfo candidate{};
      candidate.suffix = suffix;
      if (!slot_path(suffix, candidate.path)) { saw_io = true; continue; }
      std::size_t size{0U};
      if (!read_file(candidate.path.data(), frame_.data(), size)) {
        std::FILE* file = std::fopen(candidate.path.data(), "rb");
        if (file != nullptr) { std::fclose(file); saw_io = true; }
        continue;
      }
      uint64_t generation{0U};
      if (!detail::frame_info(frame_.data(), size, generation)) { saw_corrupt = true; continue; }
      if (!selected.valid || generation > selected.generation) {
        candidate.valid = true;
        candidate.generation = generation;
        selected = candidate;
      }
    }
    if (selected.valid) { state_ = DeviceControlJournalState::Valid; return true; }
    state_ = saw_io ? DeviceControlJournalState::IoError
                    : saw_corrupt ? DeviceControlJournalState::Corrupt
                                  : DeviceControlJournalState::Empty;
    return false;
  }

  std::array<char, kDeviceControlJournalPathBytes> path_{};
  mutable std::array<uint8_t, kDeviceControlJournalFrameBytes> frame_{};
  mutable std::size_t frame_size_{0U};
  mutable DeviceControlJournalState state_{DeviceControlJournalState::Empty};
};
#endif

#ifdef ARDUINO
class FsDeviceControlResultJournal final : public IDeviceControlResultJournal {
 public:
  DeviceControlJournalState state() const noexcept { return state_; }

  bool begin(fs::FS& filesystem, const char* path = "/sigap/device-control.result") noexcept {
    filesystem_ = &filesystem;
    if (path == nullptr || std::strlen(path) + 3U >= path_.size()) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    std::strncpy(path_.data(), path, path_.size() - 1U);
    path_[path_.size() - 1U] = '\0';
    // Do not decode into large command/result objects on Arduino's loopTask
    // stack.  Poller::begin() performs the authoritative decode into its
    // instance-owned buffers after recovery.  The bounded slot scan here
    // validates the journal frame and classifies empty/corrupt/I/O states.
    SlotInfo selected{};
    (void)scan(selected);
    return state_ != DeviceControlJournalState::IoError &&
           state_ != DeviceControlJournalState::Corrupt;
  }

  bool recover() noexcept override {
    if (filesystem_ == nullptr) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    SlotInfo selected{};
    return scan(selected) || state_ == DeviceControlJournalState::Empty;
  }

  bool load(DeviceControlCommand& command, DeviceControlResult& result) const noexcept override {
    SlotInfo selected{};
    if (!scan(selected) || !selected.valid) return false;
    if (!read_slot(selected.path.data(), frame_size_)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    if (!detail::decode(frame_.data(), frame_size_, command, result)) {
      state_ = DeviceControlJournalState::Corrupt;
      return false;
    }
    state_ = DeviceControlJournalState::Valid;
    return true;
  }

  bool save(const DeviceControlCommand& command, const DeviceControlResult& result) noexcept override {
    SlotInfo selected{};
    (void)scan(selected);
    if (!selected.valid && state_ != DeviceControlJournalState::Empty) return false;
    const uint64_t generation = selected.valid ? selected.generation + 1U : 1U;
    std::size_t encoded{0U};
    if (!detail::encode(command, result, generation, frame_.data(), frame_.size(), encoded)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    std::array<char, kDeviceControlJournalPathBytes> target{};
    if (!slot_path(selected.valid && selected.suffix == 'a' ? 'b' : 'a', target) ||
        !write_slot(target.data(), encoded)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    if (!read_slot(target.data(), frame_size_)) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    uint64_t verified_generation{0U};
    if (!detail::frame_info(frame_.data(), frame_size_, verified_generation) ||
        verified_generation != generation) {
      state_ = DeviceControlJournalState::Corrupt;
      return false;
    }
    state_ = DeviceControlJournalState::Valid;
    return true;
  }

  bool clear() noexcept override {
    if (filesystem_ == nullptr || !remove_if_present('a') || !remove_if_present('b')) {
      state_ = DeviceControlJournalState::IoError;
      return false;
    }
    state_ = DeviceControlJournalState::Empty;
    return true;
  }

 private:
  struct SlotInfo final {
    bool valid{false};
    char suffix{0};
    uint64_t generation{0U};
    std::array<char, kDeviceControlJournalPathBytes> path{};
  };
  bool slot_path(char suffix, std::array<char, kDeviceControlJournalPathBytes>& out) const noexcept {
    const int written = std::snprintf(out.data(), out.size(), "%s.%c", path_.data(), suffix);
    return written > 0 && static_cast<std::size_t>(written) < out.size();
  }
  bool read_slot(const char* path, std::size_t& size) const noexcept {
    if (filesystem_ == nullptr) return false;
    File file = filesystem_->open(path, FILE_READ);
    if (!file) return false;
    const std::size_t length = file.size();
    if (length == 0U || length > frame_.size()) { file.close(); return false; }
    size = file.read(frame_.data(), length);
    file.close();
    return size == length;
  }
  bool write_slot(const char* path, std::size_t size) noexcept {
    if (filesystem_ == nullptr) return false;
    File file = filesystem_->open(path, FILE_WRITE);
    if (!file) return false;
    const bool written = file.write(frame_.data(), size) == size;
    if (written) file.flush();
    file.close();
    return written;
  }
  bool remove_if_present(char suffix) noexcept {
    if (filesystem_ == nullptr) return false;
    std::array<char, kDeviceControlJournalPathBytes> path{};
    if (!slot_path(suffix, path)) return false;
    if (!filesystem_->exists(path.data())) return true;
    return filesystem_->remove(path.data());
  }
  bool scan(SlotInfo& selected) const noexcept {
    selected = SlotInfo{};
    bool saw_corrupt{false};
    bool saw_io{false};
    for (const char suffix : {'a', 'b'}) {
      std::array<char, kDeviceControlJournalPathBytes> path{};
      if (!slot_path(suffix, path)) { saw_io = true; continue; }
      if (filesystem_ == nullptr) { saw_io = true; continue; }
      if (!filesystem_->exists(path.data())) continue;
      File file = filesystem_->open(path.data(), FILE_READ);
      if (!file) { saw_io = true; continue; }
      const std::size_t length = file.size();
      if (length == 0U || length > frame_.size()) {
        file.close(); saw_corrupt = true; continue;
      }
      if (file.read(frame_.data(), length) != length) {
        file.close(); saw_io = true; continue;
      }
      file.close();
      uint64_t generation{0U};
      if (!detail::frame_info(frame_.data(), length, generation)) { saw_corrupt = true; continue; }
      if (!selected.valid || generation > selected.generation) {
        selected.valid = true;
        selected.suffix = suffix;
        selected.generation = generation;
        selected.path = path;
      }
    }
    if (selected.valid) { state_ = DeviceControlJournalState::Valid; return true; }
    state_ = saw_io ? DeviceControlJournalState::IoError
                    : saw_corrupt ? DeviceControlJournalState::Corrupt
                                  : DeviceControlJournalState::Empty;
    return false;
  }

  fs::FS* filesystem_{nullptr};
  std::array<char, kDeviceControlJournalPathBytes> path_{};
  mutable std::array<uint8_t, kDeviceControlJournalFrameBytes> frame_{};
  mutable std::size_t frame_size_{0U};
  mutable DeviceControlJournalState state_{DeviceControlJournalState::Empty};
};
#endif

}  // namespace sigap::network
