#pragma once

#include <array>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string_view>

#include "archive_store.h"

#ifdef ARDUINO
#include <FS.h>
#endif

namespace sigap::storage {

// Filesystem adapters never format and only report a record durable after the
// complete framed record has been written and flushed. Recovery scans complete
// frames and ignores a partial/corrupt tail.
class LittleFsArchiveStore : public IArchiveStore {
 public:
  explicit LittleFsArchiveStore(const char* path = "/sigap/archive.bin") noexcept
      : path_(path) {}

#ifdef ARDUINO
  bool begin(fs::FS& filesystem, uint64_t quota_bytes) noexcept {
    filesystem_ = &filesystem;
    quota_bytes_ = quota_bytes;
    if (!filesystem_->exists("/sigap")) (void)filesystem_->mkdir("/sigap");
    return recover();
  }
#else
  bool begin(uint64_t quota_bytes) noexcept {
    quota_bytes_ = quota_bytes;
    return false;
  }
#endif

  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr || !RecordCodec::valid(record)) {
      return AppendStatus::Retryable;
    }
    const auto* existing = find_index(record.identity);
    if (existing != nullptr) {
      return *existing == record.hash ? AppendStatus::Durable
                                      : AppendStatus::Invalid;
    }
    if (index_count_ == index_.size()) {
      health_ = ArchiveHealth::Blocked;
      return AppendStatus::LoggingBlocked;
    }
    if (pending_bytes_ > quota_bytes_ ||
        record.size > quota_bytes_ - pending_bytes_) {
      health_ = ArchiveHealth::Blocked;
      return AppendStatus::LoggingBlocked;
    }
    File file = filesystem_->open(path_, FILE_APPEND);
    if (!file) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    const std::size_t written = file.write(record.bytes.data(), record.size);
    file.flush();
    file.close();
    if (written != record.size) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    index_[index_count_] = record.identity;
    hashes_[index_count_] = record.hash;
    ++index_count_;
    pending_bytes_ += record.size;
    health_ = ArchiveHealth::Healthy;
    return AppendStatus::Durable;
#else
    (void)record;
    return AppendStatus::Retryable;
#endif
  }

  bool checkpoint() noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr) return false;
    File marker = filesystem_->open(checkpoint_path_, FILE_WRITE);
    if (!marker) return false;
    const uint64_t value = pending_bytes_;
    const bool written = marker.write(
        reinterpret_cast<const uint8_t*>(&value), sizeof(value)) == sizeof(value);
    marker.flush();
    marker.close();
    return written;
#else
    return false;
#endif
  }

  bool recover() noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr) return false;
    pending_bytes_ = 0U;
    index_count_ = 0U;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) {
      if (!filesystem_->exists(path_)) return restore_from_quarantine();
      health_ = ArchiveHealth::Degraded;
      return false;
    }
    std::size_t valid_bytes = 0U;
    bool clean = true;
    while (file.available()) {
      ImmutableRecordBytes record{};
      if (file.read(record.bytes.data(), kArchiveHeaderBytes) !=
          static_cast<int>(kArchiveHeaderBytes)) {
        clean = false;
        break;
      }
      const uint32_t payload_size = read_u32(record.bytes.data() + 8U);
      if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                             kArchiveTrailerBytes) {
        clean = false;
        break;
      }
      record.size = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
      const std::size_t remaining = record.size - kArchiveHeaderBytes;
      if (file.read(record.bytes.data() + kArchiveHeaderBytes, remaining) !=
          static_cast<int>(remaining)) {
        clean = false;
        break;
      }
      record.committed = true;
      if (!RecordCodec::valid(record)) {
        clean = false;
        break;
      }
      record.identity.kind = static_cast<ArchiveRecordKind>(
          record.bytes[kArchiveKindOffset]);
      record.identity.sequence = read_u64(record.bytes.data() +
                                          kArchiveSequenceOffset);
      record.identity.device_id.present = true;
      record.identity.boot_id.present = true;
      std::memcpy(record.identity.device_id.bytes.data(),
                  record.bytes.data() + kArchiveDeviceIdOffset,
                  record.identity.device_id.bytes.size());
      std::memcpy(record.identity.boot_id.bytes.data(),
                  record.bytes.data() + kArchiveBootIdOffset,
                  record.identity.boot_id.bytes.size());
      record.hash = telemetry_contract::sha256(std::string_view(
          reinterpret_cast<const char*>(record.bytes.data()), record.size));
      if (const auto* existing = find_index(record.identity);
          existing != nullptr) {
        clean = *existing == record.hash;
        break;
      }
      if (index_count_ == index_.size()) {
        clean = false;
        break;
      }
      index_[index_count_] = record.identity;
      hashes_[index_count_] = record.hash;
      ++index_count_;
      pending_bytes_ += record.size;
      valid_bytes += record.size;
    }
    file.close();
    const bool ok = clean || quarantine_tail(valid_bytes);
    health_ = ok ? ArchiveHealth::Healthy : ArchiveHealth::Degraded;
    return ok;
#else
    return false;
#endif
  }

  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t pending_quota_bytes() const noexcept override { return quota_bytes_; }
  ArchiveHealth health() const noexcept override {
#ifdef ARDUINO
    return filesystem_ == nullptr ? ArchiveHealth::Degraded : health_;
#else
    return ArchiveHealth::Degraded;
#endif
  }

 protected:
#ifdef ARDUINO
  fs::FS* filesystem_{nullptr};
#endif
  const char* path_{nullptr};
  const char* checkpoint_path_{"/sigap/archive.ckp"};
  uint64_t quota_bytes_{0U};
  uint64_t pending_bytes_{0U};

 private:
#ifdef ARDUINO
  const config::ConfigHash* find_index(const ArchiveIdentity& identity) const noexcept {
    for (std::size_t i = 0U; i < index_count_; ++i) {
      if (index_[i].kind == identity.kind &&
          index_[i].sequence == identity.sequence &&
          index_[i].device_id.bytes == identity.device_id.bytes &&
          index_[i].boot_id.bytes == identity.boot_id.bytes) {
        return &hashes_[i];
      }
    }
    return nullptr;
  }
  bool make_quarantine_path(char* output, std::size_t size) noexcept {
    if (filesystem_ == nullptr || output == nullptr) return false;
    for (unsigned suffix = 0U; suffix < 4U; ++suffix) {
      const int written = std::snprintf(output, size, "%s.tail%u",
                                        path_, suffix);
      if (written <= 0 || static_cast<std::size_t>(written) >= size) {
        return false;
      }
      if (!filesystem_->exists(output)) return true;
    }
    return false;
  }
  bool restore_from_quarantine() noexcept {
    if (filesystem_ == nullptr) return true;
    for (unsigned suffix = 0U; suffix < 4U; ++suffix) {
      char quarantine[64]{};
      const int written = std::snprintf(quarantine, sizeof(quarantine),
                                        "%s.tail%u", path_, suffix);
      if (written <= 0 ||
          static_cast<std::size_t>(written) >= sizeof(quarantine) ||
          !filesystem_->exists(quarantine)) {
        continue;
      }
      if (!copy_valid_prefix(quarantine, path_, SIZE_MAX)) {
        health_ = ArchiveHealth::Degraded;
        return false;
      }
      return recover();
    }
    return true;
  }
  bool quarantine_tail(std::size_t valid_bytes) noexcept {
    if (filesystem_ == nullptr) return false;
    char quarantine[64]{};
    if (!make_quarantine_path(quarantine, sizeof(quarantine))) return false;
    if (!filesystem_->rename(path_, quarantine)) return false;
    return copy_valid_prefix(quarantine, path_, valid_bytes);
  }
  bool copy_valid_prefix(const char* source_path,
                         const char* destination_path,
                         std::size_t valid_bytes) noexcept {
    File source = filesystem_->open(source_path, FILE_READ);
    File destination = filesystem_->open(destination_path, FILE_WRITE);
    if (!source || !destination) {
      if (source) source.close();
      if (destination) destination.close();
      return false;
    }
    if (valid_bytes == SIZE_MAX) valid_bytes = source.size();
    std::size_t remaining = valid_bytes;
    bool copied = true;
    while (remaining != 0U) {
      const std::size_t chunk =
          remaining < copy_buffer_.size() ? remaining : copy_buffer_.size();
      if (source.read(copy_buffer_.data(), chunk) != static_cast<int>(chunk) ||
          destination.write(copy_buffer_.data(), chunk) != chunk) {
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
  static uint64_t read_u64(const uint8_t* bytes) noexcept {
    uint64_t value{0U};
    for (std::size_t i = 0U; i < 8U; ++i) {
      value |= static_cast<uint64_t>(bytes[i]) << (i * 8U);
    }
    return value;
  }
  static uint32_t read_u32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0U]) |
           (static_cast<uint32_t>(bytes[1U]) << 8U) |
           (static_cast<uint32_t>(bytes[2U]) << 16U) |
           (static_cast<uint32_t>(bytes[3U]) << 24U);
  }
  std::array<ArchiveIdentity, 128U> index_{};
  std::array<config::ConfigHash, 128U> hashes_{};
  std::array<uint8_t, 256U> copy_buffer_{};
  std::size_t index_count_{0U};
  ArchiveHealth health_{ArchiveHealth::Healthy};
#endif
};

class SdArchiveStore final : public LittleFsArchiveStore {
 public:
  SdArchiveStore() noexcept : LittleFsArchiveStore("/sigap/archive-sd.bin") {}
};

class MediaFailoverStore final : public IArchiveStore {
 public:
  MediaFailoverStore(IArchiveStore& primary, IArchiveStore& fallback) noexcept
      : primary_(primary), fallback_(fallback) {}

  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
    const auto primary_status = primary_.append(record);
    if (primary_status == AppendStatus::Durable) return primary_status;
    const auto fallback_status = fallback_.append(record);
    if (fallback_status == AppendStatus::Durable) return fallback_status;
    return fallback_status == AppendStatus::Invalid ? primary_status
                                                      : fallback_status;
  }

  bool checkpoint() noexcept override {
    const bool primary = primary_.checkpoint();
    const bool fallback = fallback_.checkpoint();
    return primary || fallback;
  }

  bool recover() noexcept override {
    const bool primary = primary_.recover();
    const bool fallback = fallback_.recover();
    return primary || fallback;
  }

  uint64_t pending_bytes() const noexcept override {
    return primary_.pending_bytes() + fallback_.pending_bytes();
  }

  uint64_t pending_quota_bytes() const noexcept override {
    return primary_.pending_quota_bytes() + fallback_.pending_quota_bytes();
  }

  ArchiveHealth health() const noexcept override {
    if (primary_.health() == ArchiveHealth::Healthy) return ArchiveHealth::Healthy;
    return fallback_.health() == ArchiveHealth::Healthy ? ArchiveHealth::Degraded
                                                         : ArchiveHealth::Blocked;
  }

 private:
  IArchiveStore& primary_;
  IArchiveStore& fallback_;
};

#ifndef ARDUINO
// Native filesystem double and adapter exercise the same append-boundary
// rules without touching host files or target media.
template <std::size_t Capacity = 64U, std::size_t Bytes = 256U * 1024U>
class ArchiveFileSystemDouble final {
 public:
  bool append(const uint8_t* data, std::size_t size) noexcept {
    if (open_failure_ || short_write_ || data == nullptr ||
        size > Bytes - size_) return false;
    std::memcpy(bytes_.data() + size_, data, size);
    size_ += size;
    present_ = true;
    return true;
  }
  bool flush() noexcept { return !flush_failure_; }
  bool read(std::size_t offset, uint8_t* data, std::size_t size) const noexcept {
    if (data == nullptr || offset > size_ || size > size_ - offset) return false;
    std::memcpy(data, bytes_.data() + offset, size);
    return true;
  }
  bool truncate(std::size_t size) noexcept {
    if (flush_failure_ || size > size_) return false;
    size_ = size;
    return true;
  }
  std::size_t size() const noexcept { return size_; }
  bool exists() const noexcept { return present_; }
  bool open_failed() const noexcept { return open_failure_; }
  void set_open_failure(bool value) noexcept { open_failure_ = value; }
  void set_missing() noexcept {
    present_ = false;
    size_ = 0U;
  }
  void set_short_write(bool value) noexcept { short_write_ = value; }
  void set_flush_failure(bool value) noexcept { flush_failure_ = value; }
  void append_partial_byte(uint8_t value) noexcept {
    if (size_ < Bytes) bytes_[size_++] = value;
  }

 private:
  std::array<uint8_t, Bytes> bytes_{};
  std::size_t size_{0U};
  bool present_{false};
  bool open_failure_{false};
  bool short_write_{false};
  bool flush_failure_{false};
};

template <std::size_t Capacity = 64U, std::size_t Bytes = 256U * 1024U>
class NativeArchiveFileStore final : public IArchiveStore {
 public:
  explicit NativeArchiveFileStore(ArchiveFileSystemDouble<Capacity, Bytes>& fs) noexcept
      : fs_(fs) {}

  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
    if (!RecordCodec::valid(record)) return AppendStatus::Invalid;
    for (std::size_t i = 0U; i < count_; ++i) {
      if (same_identity(index_[i], record)) {
        return hashes_[i] == record.hash ? AppendStatus::Durable
                                         : AppendStatus::Invalid;
      }
    }
    if (pending_bytes_ > quota_bytes_ ||
        record.size > quota_bytes_ - pending_bytes_ || count_ == Capacity) {
      return AppendStatus::LoggingBlocked;
    }
    if (!fs_.append(record.bytes.data(), record.size) || !fs_.flush()) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    index_[count_] = record.identity;
    hashes_[count_] = record.hash;
    ++count_;
    pending_bytes_ += record.size;
    health_ = ArchiveHealth::Healthy;
    return AppendStatus::Durable;
  }

  bool checkpoint() noexcept override { return fs_.flush(); }

  bool recover() noexcept override {
    count_ = 0U;
    pending_bytes_ = 0U;
    if (fs_.open_failed()) {
      health_ = ArchiveHealth::Blocked;
      return false;
    }
    if (!fs_.exists()) return true;
    std::size_t offset = 0U;
    std::array<uint8_t, kArchiveHeaderBytes> header{};
    bool clean = true;
    while (offset < fs_.size()) {
      if (fs_.size() - offset < kArchiveHeaderBytes ||
          !fs_.read(offset, header.data(), header.size())) {
        clean = false;
        break;
      }
      const std::size_t payload_size = read_u32(header.data() + kArchivePayloadSizeOffset);
      const std::size_t total = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
      if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                             kArchiveTrailerBytes ||
          total > fs_.size() - offset || count_ == Capacity) {
        clean = false;
        break;
      }
      ImmutableRecordBytes record{};
      record.size = total;
      if (!fs_.read(offset, record.bytes.data(), total)) {
        clean = false;
        break;
      }
      record.committed = true;
      if (!RecordCodec::valid(record)) {
        clean = false;
        break;
      }
      record.identity.kind = static_cast<ArchiveRecordKind>(record.bytes[kArchiveKindOffset]);
      record.identity.sequence = read_u64(record.bytes.data() + kArchiveSequenceOffset);
      record.identity.device_id.present = true;
      record.identity.boot_id.present = true;
      std::memcpy(record.identity.device_id.bytes.data(),
                  record.bytes.data() + kArchiveDeviceIdOffset,
                  record.identity.device_id.bytes.size());
      std::memcpy(record.identity.boot_id.bytes.data(),
                  record.bytes.data() + kArchiveBootIdOffset,
                  record.identity.boot_id.bytes.size());
      record.hash = telemetry_contract::sha256(std::string_view(
          reinterpret_cast<const char*>(record.bytes.data()), record.size));
      index_[count_] = record.identity;
      hashes_[count_] = record.hash;
      ++count_;
      pending_bytes_ += total;
      offset += total;
    }
    if (!clean && !fs_.truncate(offset)) {
      health_ = ArchiveHealth::Degraded;
      return false;
    }
    health_ = ArchiveHealth::Healthy;
    return true;
  }

  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t pending_quota_bytes() const noexcept override { return quota_bytes_; }
  ArchiveHealth health() const noexcept override { return health_; }
  void set_quota(uint64_t quota) noexcept { quota_bytes_ = quota; }
  std::size_t size() const noexcept { return count_; }
  const config::ConfigHash* hash_at(std::size_t index) const noexcept {
    return index < count_ ? &hashes_[index] : nullptr;
  }

 private:
  static bool same_identity(const ArchiveIdentity& left,
                            const ImmutableRecordBytes& right) noexcept {
    return left.kind == right.identity.kind && left.sequence == right.identity.sequence &&
           left.device_id.bytes == right.identity.device_id.bytes &&
           left.boot_id.bytes == right.identity.boot_id.bytes;
  }
  static uint32_t read_u32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0U]) |
           (static_cast<uint32_t>(bytes[1U]) << 8U) |
           (static_cast<uint32_t>(bytes[2U]) << 16U) |
           (static_cast<uint32_t>(bytes[3U]) << 24U);
  }
  static uint64_t read_u64(const uint8_t* bytes) noexcept {
    uint64_t value{0U};
    for (std::size_t i = 0U; i < 8U; ++i) value |= static_cast<uint64_t>(bytes[i]) << (i * 8U);
    return value;
  }
  ArchiveFileSystemDouble<Capacity, Bytes>& fs_;
  std::array<ArchiveIdentity, Capacity> index_{};
  std::array<config::ConfigHash, Capacity> hashes_{};
  uint64_t quota_bytes_{Bytes};
  uint64_t pending_bytes_{0U};
  std::size_t count_{0U};
  ArchiveHealth health_{ArchiveHealth::Healthy};
};
#endif

}  // namespace sigap::storage
