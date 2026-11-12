#pragma once

#include <array>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string_view>

#include "archive_store.h"
#include "littlefs_archive_store.h"

#ifdef ARDUINO
#include <FS.h>
#include <sys/stat.h>
#endif

namespace sigap::storage {

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

  AppendStatus append_gap(const GapRange& range) noexcept override {
    const auto primary_status = primary_.append_gap(range);
    if (primary_status == AppendStatus::Durable) return primary_status;
    const auto fallback_status = fallback_.append_gap(range);
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

  bool peek_pending(ImmutableRecordBytes& output) noexcept override {
    if (primary_.peek_pending(output)) return true;
    return fallback_.peek_pending(output);
  }

  bool reclaim_pending(const ArchiveIdentity& identity,
                       const config::ConfigHash& hash) noexcept override {
    if (primary_.reclaim_pending(identity, hash)) return true;
    return fallback_.reclaim_pending(identity, hash);
  }

  bool contains(const ArchiveIdentity& identity,
                const config::ConfigHash& hash) noexcept override {
    return primary_.contains(identity, hash) || fallback_.contains(identity, hash);
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
  bool remove_prefix(std::size_t prefix) noexcept {
    if (prefix > size_) return false;
    std::memmove(bytes_.data(), bytes_.data() + prefix, size_ - prefix);
    size_ -= prefix;
    return true;
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
    if (const auto* existing = find_cached(record.identity);
        existing != nullptr) {
      return *existing == record.hash ? AppendStatus::Durable
                                      : AppendStatus::Invalid;
    }
    if (cache_count_ == Capacity) {
      bool read_ok = true;
      config::ConfigHash disk_hash{};
      if (find_on_disk(record.identity, disk_hash, read_ok)) {
        return disk_hash == record.hash ? AppendStatus::Durable
                                        : AppendStatus::Invalid;
      }
      if (!read_ok) {
        health_ = ArchiveHealth::Degraded;
        return AppendStatus::Retryable;
      }
    }
    if (pending_bytes_ > quota_bytes_ ||
        record.size > quota_bytes_ - pending_bytes_) {
      return AppendStatus::LoggingBlocked;
    }
    if (!fs_.append(record.bytes.data(), record.size) || !fs_.flush()) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    if (cache_count_ < Capacity) {
      index_[cache_count_] = record.identity;
      hashes_[cache_count_] = record.hash;
      ++cache_count_;
    }
    ++record_count_;
    pending_bytes_ += record.size;
    health_ = ArchiveHealth::Healthy;
    return AppendStatus::Durable;
  }

  bool checkpoint() noexcept override { return fs_.flush(); }

  bool recover() noexcept override {
    cache_count_ = 0U;
    record_count_ = 0U;
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
          total > fs_.size() - offset) {
        clean = false;
        break;
      }
      ImmutableRecordBytes& record = scan_record_;
      record = ImmutableRecordBytes{};
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
      decode_identity(record);
      record.hash = telemetry_contract::sha256(std::string_view(
          reinterpret_cast<const char*>(record.bytes.data()), record.size));
      if (const auto* existing = find_cached(record.identity);
          existing != nullptr) {
        if (*existing != record.hash) health_ = ArchiveHealth::Degraded;
        offset += total;
        continue;
      }
      if (cache_count_ == Capacity) {
        bool read_ok = true;
        config::ConfigHash existing_hash{};
        if (find_on_disk(record.identity, existing_hash, read_ok, offset)) {
          if (existing_hash != record.hash) health_ = ArchiveHealth::Degraded;
          offset += total;
          continue;
        }
        if (!read_ok) {
          clean = false;
          break;
        }
      }
      if (cache_count_ < Capacity) {
        index_[cache_count_] = record.identity;
        hashes_[cache_count_] = record.hash;
        ++cache_count_;
      }
      ++record_count_;
      pending_bytes_ += total;
      offset += total;
    }
    if (!clean && !fs_.truncate(offset)) {
      health_ = ArchiveHealth::Degraded;
      return false;
    }
    if (health_ != ArchiveHealth::Degraded) {
      health_ = ArchiveHealth::Healthy;
    }
    return true;
  }

  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t pending_quota_bytes() const noexcept override { return quota_bytes_; }
  ArchiveHealth health() const noexcept override { return health_; }
  void set_quota(uint64_t quota) noexcept { quota_bytes_ = quota; }
  std::size_t size() const noexcept { return record_count_; }
  const config::ConfigHash* hash_at(std::size_t index) const noexcept {
    return index < cache_count_ ? &hashes_[index] : nullptr;
  }
  bool peek_pending(ImmutableRecordBytes& output) noexcept override {
    if (!fs_.exists() || fs_.size() < kArchiveHeaderBytes) return false;
    if (!read_record_at(0U, output)) return false;
    return true;
  }
  bool reclaim_pending(const ArchiveIdentity& identity,
                       const config::ConfigHash& hash) noexcept override {
    ImmutableRecordBytes record{};
    if (!read_record_at(0U, record) || !same_identity(record.identity, identity) ||
        record.hash != hash || !fs_.remove_prefix(record.size)) {
      return false;
    }
    return recover();
  }
  bool contains(const ArchiveIdentity& identity,
                const config::ConfigHash& hash) noexcept override {
    bool read_ok = true;
    config::ConfigHash existing{};
    return find_on_disk(identity, existing, read_ok) && existing == hash;
  }

 private:
  static bool same_identity(const ArchiveIdentity& left,
                            const ArchiveIdentity& right) noexcept {
    return left.kind == right.kind && left.sequence == right.sequence &&
           left.device_id.bytes == right.device_id.bytes &&
           left.boot_id.bytes == right.boot_id.bytes;
  }
  static bool same_identity(const ArchiveIdentity& left,
                            const ImmutableRecordBytes& right) noexcept {
    return same_identity(left, right.identity);
  }
  static void decode_identity(ImmutableRecordBytes& record) noexcept {
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
  }
  const config::ConfigHash* find_cached(const ArchiveIdentity& identity) const noexcept {
    for (std::size_t i = 0U; i < cache_count_; ++i) {
      if (same_identity(index_[i], identity)) return &hashes_[i];
    }
    return nullptr;
  }
  bool find_on_disk(const ArchiveIdentity& identity,
                    config::ConfigHash& hash,
                    bool& read_ok,
                    std::size_t limit = SIZE_MAX) noexcept {
    read_ok = true;
    std::size_t offset = 0U;
    while (offset < fs_.size() && offset < limit) {
      if (limit - offset < kArchiveHeaderBytes ||
          fs_.size() - offset < kArchiveHeaderBytes ||
          !fs_.read(offset, header_.data(), header_.size())) {
        read_ok = false;
        return false;
      }
      const std::size_t payload_size = read_u32(header_.data() + kArchivePayloadSizeOffset);
      const std::size_t total = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
      if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes - kArchiveTrailerBytes ||
          total > fs_.size() - offset || total > limit - offset ||
          !fs_.read(offset, lookup_record_.bytes.data(), total)) {
        read_ok = false;
        return false;
      }
      lookup_record_.size = total;
      lookup_record_.committed = true;
      if (!RecordCodec::valid(lookup_record_)) {
        read_ok = false;
        return false;
      }
      decode_identity(lookup_record_);
      lookup_record_.hash = telemetry_contract::sha256(std::string_view(
          reinterpret_cast<const char*>(lookup_record_.bytes.data()), total));
      if (same_identity(lookup_record_.identity, identity)) {
        hash = lookup_record_.hash;
        return true;
      }
      offset += total;
    }
    return false;
  }
  bool read_record_at(std::size_t offset,
                      ImmutableRecordBytes& record) noexcept {
    if (offset > fs_.size() || fs_.size() - offset < kArchiveHeaderBytes ||
        !fs_.read(offset, header_.data(), header_.size())) return false;
    const std::size_t payload_size =
        read_u32(header_.data() + kArchivePayloadSizeOffset);
    const std::size_t total = kArchiveHeaderBytes + payload_size +
                              kArchiveTrailerBytes;
    if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                           kArchiveTrailerBytes ||
        total > fs_.size() - offset ||
        !fs_.read(offset, record.bytes.data(), total)) return false;
    record.size = total;
    record.committed = true;
    if (!RecordCodec::valid(record)) return false;
    decode_identity(record);
    record.hash = telemetry_contract::sha256(std::string_view(
        reinterpret_cast<const char*>(record.bytes.data()), total));
    return true;
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
  std::array<uint8_t, kArchiveHeaderBytes> header_{};
  ImmutableRecordBytes scan_record_{};
  ImmutableRecordBytes lookup_record_{};
  uint64_t quota_bytes_{Bytes};
  uint64_t pending_bytes_{0U};
  std::size_t cache_count_{0U};
  std::size_t record_count_{0U};
  ArchiveHealth health_{ArchiveHealth::Healthy};
};
#endif

}  // namespace sigap::storage
