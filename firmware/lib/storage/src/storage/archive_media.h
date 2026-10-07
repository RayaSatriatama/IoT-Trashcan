#pragma once

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
    if (pending_bytes_ > quota_bytes_ ||
        record.size > quota_bytes_ - pending_bytes_) {
      return AppendStatus::LoggingBlocked;
    }
    File file = filesystem_->open(path_, FILE_APPEND);
    if (!file) return AppendStatus::Retryable;
    const std::size_t written = file.write(record.bytes.data(), record.size);
    file.flush();
    file.close();
    if (written != record.size) return AppendStatus::Retryable;
    pending_bytes_ += record.size;
    return AppendStatus::Durable;
#else
    (void)record;
    return AppendStatus::Retryable;
#endif
  }

  bool checkpoint() noexcept override {
#ifdef ARDUINO
    return filesystem_ != nullptr;
#else
    return false;
#endif
  }

  bool recover() noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr) return false;
    pending_bytes_ = 0U;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) return true;
    while (file.available()) {
      ImmutableRecordBytes record{};
      if (file.read(record.bytes.data(), kArchiveHeaderBytes) !=
          static_cast<int>(kArchiveHeaderBytes)) {
        break;
      }
      const uint32_t payload_size = read_u32(record.bytes.data() + 8U);
      if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                             kArchiveTrailerBytes) {
        break;
      }
      record.size = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
      const std::size_t remaining = record.size - kArchiveHeaderBytes;
      if (file.read(record.bytes.data() + kArchiveHeaderBytes, remaining) !=
          static_cast<int>(remaining)) {
        break;
      }
      record.committed = true;
      if (!RecordCodec::valid(record)) break;
      pending_bytes_ += record.size;
    }
    file.close();
    return true;
#else
    return false;
#endif
  }

  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t pending_quota_bytes() const noexcept override { return quota_bytes_; }
  ArchiveHealth health() const noexcept override {
#ifdef ARDUINO
    return filesystem_ == nullptr ? ArchiveHealth::Degraded
                                   : ArchiveHealth::Healthy;
#else
    return ArchiveHealth::Degraded;
#endif
  }

 protected:
#ifdef ARDUINO
  fs::FS* filesystem_{nullptr};
#endif
  const char* path_{nullptr};
  uint64_t quota_bytes_{0U};
  uint64_t pending_bytes_{0U};

 private:
#ifdef ARDUINO
  static uint32_t read_u32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0U]) |
           (static_cast<uint32_t>(bytes[1U]) << 8U) |
           (static_cast<uint32_t>(bytes[2U]) << 16U) |
           (static_cast<uint32_t>(bytes[3U]) << 24U);
  }
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

}  // namespace sigap::storage
