#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "immutable_batch.h"
#include "storage/archive_codec.h"

#ifdef ARDUINO
#include <FS.h>
#endif

namespace sigap::telemetry_archive {

inline constexpr uint32_t kAckJournalMagic{0x5347414AU};
inline constexpr uint8_t kAckJournalVersion{1U};
inline constexpr uint8_t kAckOpPending{1U};
inline constexpr uint8_t kAckOpServerAcked{2U};
inline constexpr uint8_t kAckOpReclaimed{3U};
inline constexpr uint32_t kAckJournalMarker{0xACCE551U};
inline constexpr std::size_t kAckJournalIdBytes{96U};
inline constexpr std::size_t kAckJournalDeviceIdBytes{16U};
inline constexpr std::size_t kAckJournalFrameBytes{4U + 1U + 1U + 2U +
                                                    kAckJournalIdBytes + kAckJournalDeviceIdBytes + 32U +
                                                    2U + 4U + 4U};
inline constexpr std::size_t kAckJournalChecksumOffset{kAckJournalFrameBytes - 8U};
inline constexpr std::size_t kAckJournalMarkerOffset{kAckJournalFrameBytes - 4U};
static_assert(8U + kAckJournalIdBytes + kAckJournalDeviceIdBytes + 32U + 2U + 4U + 4U ==
                  kAckJournalFrameBytes,
              "ACK journal layout must remain exact");

struct AckJournalEntry final {
  std::array<char, kAckJournalIdBytes> batch_id{};
  std::size_t batch_id_size{0U};
  config::ConfigHash body_hash{};
  uint16_t record_count{0U};
  acquisition::DeviceId device_id{};
  JournalState state{JournalState::Pending};
};

enum class AckPersistStatus : uint8_t { Durable, Retryable, Invalid, NotFound };

template <std::size_t Capacity>
class MemoryAckJournalStore final {
 public:
  AckPersistStatus append_pending(const ImmutableBatch& batch) noexcept {
    if (!batch.attempted || batch.batch_id_size >= kAckJournalIdBytes) {
      return AckPersistStatus::Invalid;
    }
    for (std::size_t i = 0U; i < count_; ++i) {
      if (same_id(entries_[i], batch)) {
        return entries_[i].body_hash == batch.body_hash
                   ? AckPersistStatus::Durable
                   : AckPersistStatus::Invalid;
      }
    }
    if (count_ == Capacity) return AckPersistStatus::Retryable;
    AckJournalEntry& entry = entries_[count_++];
    std::memcpy(entry.batch_id.data(), batch.batch_id.data(), batch.batch_id_size);
    entry.batch_id_size = batch.batch_id_size;
    entry.body_hash = batch.body_hash;
    entry.record_count = batch.record_count;
    entry.device_id = batch.device_id;
    entry.state = JournalState::Pending;
    return AckPersistStatus::Durable;
  }

  AckPersistStatus apply_ack(const Ack& ack) noexcept {
    AckJournalEntry* entry = find(ack);
    if (entry == nullptr) return AckPersistStatus::NotFound;
    ImmutableBatch batch{};
    std::memcpy(batch.batch_id.data(), entry->batch_id.data(), entry->batch_id_size);
    batch.batch_id_size = entry->batch_id_size;
    batch.body_hash = entry->body_hash;
    batch.record_count = entry->record_count;
    batch.device_id = entry->device_id;
    batch.attempted = true;
    if (!ack_matches(batch, ack)) return AckPersistStatus::Invalid;
    entry->state = JournalState::ServerAcked;
    checkpointed_ = false;
    return AckPersistStatus::Durable;
  }

  bool checkpoint() noexcept {
    if (checkpoint_failure_) return false;
    checkpointed_ = true;
    return true;
  }

  std::size_t reclaim() noexcept {
    if (!checkpointed_) return 0U;
    std::size_t write{0U};
    std::size_t reclaimed{0U};
    for (std::size_t read = 0U; read < count_; ++read) {
      if (entries_[read].state == JournalState::ServerAcked) {
        ++reclaimed;
        continue;
      }
      entries_[write++] = entries_[read];
    }
    count_ = write;
    checkpointed_ = false;
    return reclaimed;
  }

  const AckJournalEntry* at(std::size_t index) const noexcept {
    return index < count_ ? &entries_[index] : nullptr;
  }
  std::size_t size() const noexcept { return count_; }
  void set_checkpoint_failure(bool value) noexcept { checkpoint_failure_ = value; }

 private:
  static bool same_id(const AckJournalEntry& entry,
                      const ImmutableBatch& batch) noexcept {
    return entry.batch_id_size == batch.batch_id_size &&
           std::equal(entry.batch_id.begin(),
                      entry.batch_id.begin() + entry.batch_id_size,
                      batch.batch_id.begin());
  }
  AckJournalEntry* find(const Ack& ack) noexcept {
    for (std::size_t i = 0U; i < count_; ++i) {
      if (entries_[i].batch_id_size == ack.batch_id_size &&
          entries_[i].device_id.bytes == ack.device_id.bytes &&
          std::equal(entries_[i].batch_id.begin(),
                     entries_[i].batch_id.begin() + entries_[i].batch_id_size,
                     ack.batch_id.begin())) return &entries_[i];
    }
    return nullptr;
  }
  std::array<AckJournalEntry, Capacity> entries_{};
  std::size_t count_{0U};
  bool checkpointed_{false};
  bool checkpoint_failure_{false};
};

#ifdef ARDUINO
class FsAckJournalStore final {
 public:
  bool begin(fs::FS& filesystem, const char* path = "/sigap/acks.bin") noexcept {
    filesystem_ = &filesystem;
    path_ = path;
    return recover();
  }

  AckPersistStatus append_pending(const ImmutableBatch& batch) noexcept {
    AckJournalEntry entry{};
    if (!make_entry(batch, entry)) return AckPersistStatus::Invalid;
    for (std::size_t i = 0U; i < count_; ++i) {
      if (same_identity(entries_[i], entry)) {
        return entries_[i].body_hash == entry.body_hash &&
                       entries_[i].record_count == entry.record_count
                   ? AckPersistStatus::Durable
                   : AckPersistStatus::Invalid;
      }
    }
    if (count_ == entries_.size()) return AckPersistStatus::Retryable;
    const auto status = append_frame(kAckOpPending, entry);
    if (status == AckPersistStatus::Durable) {
      entries_[count_++] = entry;
      ++generation_;
      checkpointed_ = false;
    }
    return status;
  }

  AckPersistStatus apply_ack(const Ack& ack) noexcept {
    AckJournalEntry* entry = find(ack);
    if (entry == nullptr) return AckPersistStatus::NotFound;
    ImmutableBatch batch{};
    std::memcpy(batch.batch_id.data(), entry->batch_id.data(), entry->batch_id_size);
    batch.batch_id_size = entry->batch_id_size;
    batch.body_hash = entry->body_hash;
    batch.record_count = entry->record_count;
    batch.device_id = entry->device_id;
    batch.attempted = true;
    if (!ack_matches(batch, ack)) return AckPersistStatus::Invalid;
    const auto status = append_frame(kAckOpServerAcked, *entry);
    if (status == AckPersistStatus::Durable) {
      entry->state = JournalState::ServerAcked;
      ++generation_;
      checkpointed_ = false;
    }
    return status;
  }

  bool checkpoint() noexcept {
    if (filesystem_ == nullptr) return false;
    File marker = filesystem_->open(checkpoint_path_, FILE_WRITE);
    if (!marker) return false;
    std::array<uint8_t, 8U> bytes{};
    write_u32(bytes.data(), kAckJournalMarker);
    write_u32(bytes.data() + 4U, generation_);
    const bool written = marker.write(bytes.data(), bytes.size()) == bytes.size();
    marker.flush();
    marker.close();
    checkpointed_ = written;
    if (checkpointed_) checkpoint_generation_ = generation_;
    return checkpointed_;
  }

  std::size_t reclaim() noexcept {
    if (filesystem_ == nullptr || !checkpointed_ ||
        checkpoint_generation_ != generation_) return 0U;
    std::size_t reclaimed{0U};
    for (std::size_t i = 0U; i < count_; ++i) {
      if (entries_[i].state == JournalState::ServerAcked &&
          append_frame(kAckOpReclaimed, entries_[i]) == AckPersistStatus::Durable) {
        remove(&entries_[i]);
        --i;
        ++reclaimed;
        ++generation_;
      }
    }
    checkpointed_ = false;
    return reclaimed;
  }

  bool recover() noexcept {
    if (filesystem_ == nullptr) return false;
    count_ = 0U;
    generation_ = 0U;
    checkpointed_ = false;
    checkpoint_generation_ = 0U;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) {
      if (filesystem_->exists(path_)) return false;
      if (!restore_from_quarantine()) return false;
      file = filesystem_->open(path_, FILE_READ);
      if (!file) return !filesystem_->exists(path_);
    }
    std::array<uint8_t, kAckJournalFrameBytes> frame{};
    std::size_t valid_bytes = 0U;
    bool clean = true;
    while (file.available()) {
      if (file.read(frame.data(), frame.size()) != static_cast<int>(frame.size()) ||
          read_u32(frame.data()) != kAckJournalMagic ||
          frame[4U] != kAckJournalVersion ||
          read_u32(frame.data() + kAckJournalMarkerOffset) != kAckJournalMarker ||
          read_u32(frame.data() + kAckJournalChecksumOffset) !=
              ack_crc32(frame.data(), kAckJournalChecksumOffset)) {
        clean = false;
        break;
      }
      const uint8_t op = frame[5U];
      if (op != kAckOpPending && op != kAckOpServerAcked &&
          op != kAckOpReclaimed) {
        clean = false;
        break;
      }
      AckJournalEntry entry{};
      entry.batch_id_size = read_u16(frame.data() + 6U);
      if (entry.batch_id_size == 0U || entry.batch_id_size >= kAckJournalIdBytes) {
        clean = false;
        break;
      }
      std::memcpy(entry.batch_id.data(), frame.data() + 8U, entry.batch_id_size);
      std::memcpy(entry.device_id.bytes.data(), frame.data() + 8U + kAckJournalIdBytes,
                  entry.device_id.bytes.size());
      entry.device_id.present = true;
      std::memcpy(entry.body_hash.data(), frame.data() + 8U + kAckJournalIdBytes +
                  kAckJournalDeviceIdBytes,
                  entry.body_hash.size());
      entry.record_count = read_u16(frame.data() + 8U + kAckJournalIdBytes +
                                    kAckJournalDeviceIdBytes + 32U);
      AckJournalEntry* current = find(entry);
      if (op == kAckOpReclaimed) {
        if (current != nullptr) remove(current);
      } else if (current == nullptr && count_ < entries_.size()) {
        entry.state = op == kAckOpServerAcked ? JournalState::ServerAcked
                                              : JournalState::Pending;
        entries_[count_++] = entry;
      } else if (current == nullptr) {
        clean = false;
        break;
      } else if (current != nullptr && op == kAckOpServerAcked) {
        current->state = JournalState::ServerAcked;
      }
      ++generation_;
      valid_bytes += frame.size();
    }
    file.close();
    if (clean) read_checkpoint();
    return clean || quarantine_tail(valid_bytes);
  }

 private:
  static uint16_t read_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0U] | (p[1U] << 8U));
  }
  static uint32_t read_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0U]) |
           (static_cast<uint32_t>(p[1U]) << 8U) |
           (static_cast<uint32_t>(p[2U]) << 16U) |
           (static_cast<uint32_t>(p[3U]) << 24U);
  }
  bool make_entry(const ImmutableBatch& batch, AckJournalEntry& entry) noexcept {
    if (!batch.attempted || !batch.device_id.present || batch.batch_id_size == 0U ||
        batch.batch_id_size >= kAckJournalIdBytes ||
        !batch.body_hash_matches()) return false;
    std::memcpy(entry.batch_id.data(), batch.batch_id.data(), batch.batch_id_size);
    entry.batch_id_size = batch.batch_id_size;
    entry.body_hash = batch.body_hash;
    entry.record_count = batch.record_count;
    entry.device_id = batch.device_id;
    return true;
  }
  static bool same_identity(const AckJournalEntry& left,
                            const AckJournalEntry& right) noexcept {
    return left.batch_id_size == right.batch_id_size &&
           left.device_id.bytes == right.device_id.bytes &&
           std::equal(left.batch_id.begin(),
                      left.batch_id.begin() + left.batch_id_size,
                      right.batch_id.begin());
  }
  AckJournalEntry* find(const AckJournalEntry& needle) noexcept {
    for (std::size_t i = 0U; i < count_; ++i) {
      if (entries_[i].batch_id_size == needle.batch_id_size &&
          entries_[i].device_id.bytes == needle.device_id.bytes &&
          std::equal(entries_[i].batch_id.begin(),
                     entries_[i].batch_id.begin() + entries_[i].batch_id_size,
                     needle.batch_id.begin())) return &entries_[i];
    }
    return nullptr;
  }
  AckJournalEntry* find(const Ack& ack) noexcept {
    if (ack.batch_id_size == 0U || ack.batch_id_size >= kAckJournalIdBytes) {
      return nullptr;
    }
    AckJournalEntry needle{};
    needle.batch_id_size = ack.batch_id_size;
    std::memcpy(needle.batch_id.data(), ack.batch_id.data(), ack.batch_id_size);
    needle.device_id = ack.device_id;
    return find(needle);
  }
  void remove(AckJournalEntry* entry) noexcept {
    const std::size_t index = static_cast<std::size_t>(entry - entries_.data());
    if (index >= count_) return;
    entries_[index] = entries_[count_ - 1U];
    --count_;
  }
  AckPersistStatus append_frame(uint8_t op, const AckJournalEntry& entry) noexcept {
    if (filesystem_ == nullptr) return AckPersistStatus::Retryable;
    File file = filesystem_->open(path_, FILE_APPEND);
    if (!file) return AckPersistStatus::Retryable;
    std::array<uint8_t, kAckJournalFrameBytes> frame{};
    frame[0U] = static_cast<uint8_t>(kAckJournalMagic);
    frame[1U] = static_cast<uint8_t>(kAckJournalMagic >> 8U);
    frame[2U] = static_cast<uint8_t>(kAckJournalMagic >> 16U);
    frame[3U] = static_cast<uint8_t>(kAckJournalMagic >> 24U);
    frame[4U] = kAckJournalVersion;
    frame[5U] = op;
    frame[6U] = static_cast<uint8_t>(entry.batch_id_size);
    frame[7U] = static_cast<uint8_t>(entry.batch_id_size >> 8U);
    std::memcpy(frame.data() + 8U, entry.batch_id.data(), entry.batch_id.size());
    std::memcpy(frame.data() + 8U + kAckJournalIdBytes, entry.device_id.bytes.data(),
                entry.device_id.bytes.size());
    std::memcpy(frame.data() + 8U + kAckJournalIdBytes + kAckJournalDeviceIdBytes,
                entry.body_hash.data(),
                entry.body_hash.size());
    frame[8U + kAckJournalIdBytes + kAckJournalDeviceIdBytes + 32U] =
        static_cast<uint8_t>(entry.record_count);
    frame[8U + kAckJournalIdBytes + kAckJournalDeviceIdBytes + 33U] =
        static_cast<uint8_t>(entry.record_count >> 8U);
    const uint32_t checksum = ack_crc32(frame.data(), kAckJournalChecksumOffset);
    write_u32(frame.data() + kAckJournalChecksumOffset, checksum);
    write_u32(frame.data() + kAckJournalMarkerOffset, kAckJournalMarker);
    const bool written = file.write(frame.data(), frame.size()) == frame.size();
    file.flush();
    file.close();
    if (!written) return AckPersistStatus::Retryable;
    return AckPersistStatus::Durable;
  }
  void read_checkpoint() noexcept {
    checkpointed_ = false;
    if (filesystem_ == nullptr) return;
    File marker = filesystem_->open(checkpoint_path_, FILE_READ);
    if (!marker) return;
    std::array<uint8_t, 8U> bytes{};
    const bool read = marker.read(bytes.data(), bytes.size()) ==
                      static_cast<int>(bytes.size());
    marker.close();
    if (read && read_u32(bytes.data()) == kAckJournalMarker) {
      checkpoint_generation_ = read_u32(bytes.data() + 4U);
      checkpointed_ = checkpoint_generation_ == generation_;
    }
  }
  static void write_u32(uint8_t* p, uint32_t value) noexcept {
    p[0U] = static_cast<uint8_t>(value);
    p[1U] = static_cast<uint8_t>(value >> 8U);
    p[2U] = static_cast<uint8_t>(value >> 16U);
    p[3U] = static_cast<uint8_t>(value >> 24U);
  }
  static uint32_t ack_crc32(const uint8_t* data, std::size_t size) noexcept {
    return sigap::storage::archive_crc32(data, size);
  }
  bool quarantine_tail(std::size_t valid_bytes) noexcept {
    if (filesystem_ == nullptr) return false;
    char quarantine[64]{};
    const int written = std::snprintf(quarantine, sizeof(quarantine),
                                      "%s.tail", path_);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(quarantine)) return false;
    if (filesystem_->exists(quarantine)) return false;
    if (!filesystem_->rename(path_, quarantine)) return false;
    File source = filesystem_->open(quarantine, FILE_READ);
    File destination = filesystem_->open(path_, FILE_WRITE);
    if (!source || !destination) {
      if (source) source.close();
      if (destination) destination.close();
      return false;
    }
    std::array<uint8_t, 256U> buffer{};
    std::size_t remaining = valid_bytes;
    bool copied = true;
    while (remaining != 0U) {
      const std::size_t chunk = remaining < buffer.size() ? remaining : buffer.size();
      if (source.read(buffer.data(), chunk) != static_cast<int>(chunk) ||
          destination.write(buffer.data(), chunk) != chunk) {
        copied = false;
        break;
      }
      remaining -= chunk;
    }
    destination.flush();
    source.close();
    destination.close();
    return copied && remaining == 0U;
  }
  bool restore_from_quarantine() noexcept {
    if (filesystem_ == nullptr) return false;
    char quarantine[64]{};
    const int written = std::snprintf(quarantine, sizeof(quarantine),
                                      "%s.tail", path_);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(quarantine) ||
        !filesystem_->exists(quarantine)) return true;
    File source = filesystem_->open(quarantine, FILE_READ);
    File destination = filesystem_->open(path_, FILE_WRITE);
    if (!source || !destination) {
      if (source) source.close();
      if (destination) destination.close();
      return false;
    }
    std::array<uint8_t, 256U> buffer{};
    std::size_t remaining = source.size();
    bool copied = true;
    while (remaining != 0U) {
      const std::size_t chunk = remaining < buffer.size() ? remaining : buffer.size();
      if (source.read(buffer.data(), chunk) != static_cast<int>(chunk) ||
          destination.write(buffer.data(), chunk) != chunk) {
        copied = false;
        break;
      }
      remaining -= chunk;
    }
    destination.flush();
    source.close();
    destination.close();
    return copied && remaining == 0U;
  }
  fs::FS* filesystem_{nullptr};
  const char* path_{nullptr};
  const char* checkpoint_path_{"/sigap/acks.ckp"};
  std::array<AckJournalEntry, 16U> entries_{};
  std::size_t count_{0U};
  bool checkpointed_{false};
  uint32_t generation_{0U};
  uint32_t checkpoint_generation_{0U};
};
#endif

}  // namespace sigap::telemetry_archive
