#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "immutable_batch.h"
#include "storage/archive_codec.h"

#ifdef ARDUINO
#include <FS.h>
#endif

namespace sigap::telemetry_archive {

inline constexpr uint32_t kBatchFrameMagic{0x53474254U};
inline constexpr uint16_t kBatchFrameVersion{1U};
inline constexpr uint32_t kBatchFrameCommitMarker{0xBA7C0EDU};
inline constexpr std::size_t kBatchFrameHeaderBytes{46U};
inline constexpr std::size_t kBatchFrameTrailerBytes{4U};

enum class BatchStoreStatus : uint8_t { Durable, Retryable, LoggingBlocked, Invalid };
enum class BatchStoreFault : uint8_t { None, ShortWrite, FlushFailure, CorruptTail };

class IImmutableBatchStore {
 public:
  virtual ~IImmutableBatchStore() = default;
  virtual BatchStoreStatus append(const ImmutableBatch& batch) noexcept = 0;
  virtual bool checkpoint() noexcept = 0;
  virtual bool recover() noexcept = 0;
  virtual uint64_t pending_bytes() const noexcept = 0;
  virtual uint64_t quota_bytes() const noexcept = 0;
};

template <std::size_t Capacity>
class MemoryImmutableBatchStore final : public IImmutableBatchStore {
 public:
  explicit MemoryImmutableBatchStore(uint64_t quota_bytes) noexcept
      : quota_bytes_(quota_bytes) {}

  BatchStoreStatus append(const ImmutableBatch& batch) noexcept override {
    if (!batch.attempted) return BatchStoreStatus::Invalid;
    if (fault_ == BatchStoreFault::ShortWrite) return BatchStoreStatus::Retryable;
    if (pending_bytes_ > quota_bytes_ ||
        batch.body_size > quota_bytes_ - pending_bytes_ || count_ == Capacity) {
      return BatchStoreStatus::LoggingBlocked;
    }
    for (std::size_t index = 0U; index < count_; ++index) {
      if (entries_[index].batch.id() == batch.id()) {
        return entries_[index].batch.body_hash == batch.body_hash
                   ? BatchStoreStatus::Durable
                   : BatchStoreStatus::Invalid;
      }
    }
    entries_[count_++].batch = batch;
    pending_bytes_ += batch.body_size;
    return BatchStoreStatus::Durable;
  }

  bool checkpoint() noexcept override {
    if (fault_ == BatchStoreFault::FlushFailure) return false;
    checkpointed_ = true;
    return true;
  }
  bool recover() noexcept override {
    if (fault_ == BatchStoreFault::CorruptTail && count_ != 0U) {
      pending_bytes_ -= entries_[count_ - 1U].batch.body_size;
      entries_[count_ - 1U] = Entry{};
      --count_;
      fault_ = BatchStoreFault::None;
    }
    return true;
  }
  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t quota_bytes() const noexcept override { return quota_bytes_; }
  void set_fault(BatchStoreFault fault) noexcept { fault_ = fault; }
  std::size_t size() const noexcept { return count_; }
  const ImmutableBatch* at(std::size_t index) const noexcept {
    return index < count_ ? &entries_[index].batch : nullptr;
  }

 private:
  struct Entry final { ImmutableBatch batch{}; };
  std::array<Entry, Capacity> entries_{};
  uint64_t quota_bytes_{0U};
  uint64_t pending_bytes_{0U};
  std::size_t count_{0U};
  BatchStoreFault fault_{BatchStoreFault::None};
  bool checkpointed_{false};
};

// Target adapter for exact immutable batch bytes. It is intentionally not
// connected to an HTTP worker in Task 6; the future worker consumes this seam.
class FsImmutableBatchStore final : public IImmutableBatchStore {
 public:
#ifdef ARDUINO
  bool begin(fs::FS& filesystem, uint64_t quota_bytes,
             const char* path = "/sigap/batches.bin") noexcept {
    filesystem_ = &filesystem;
    quota_bytes_ = quota_bytes;
    path_ = path;
    return recover();
  }
#else
  bool begin(uint64_t quota_bytes) noexcept {
    quota_bytes_ = quota_bytes;
    return false;
  }
#endif

  BatchStoreStatus append(const ImmutableBatch& batch) noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr || !batch.attempted) return BatchStoreStatus::Retryable;
    const std::size_t total = kBatchFrameHeaderBytes + batch.batch_id_size +
                              batch.body_size + kBatchFrameTrailerBytes;
    if (pending_bytes_ > quota_bytes_ || total > quota_bytes_ - pending_bytes_ ||
        total > kMaxBatchBytes + 128U) return BatchStoreStatus::LoggingBlocked;
    File file = filesystem_->open(path_, FILE_APPEND);
    if (!file) return BatchStoreStatus::Retryable;
    std::array<uint8_t, kBatchFrameHeaderBytes> header{};
    const auto write_u16 = [](uint8_t* bytes, uint16_t value) noexcept {
      bytes[0U] = static_cast<uint8_t>(value);
      bytes[1U] = static_cast<uint8_t>(value >> 8U);
    };
    const auto write_u32 = [](uint8_t* bytes, uint32_t value) noexcept {
      bytes[0U] = static_cast<uint8_t>(value);
      bytes[1U] = static_cast<uint8_t>(value >> 8U);
      bytes[2U] = static_cast<uint8_t>(value >> 16U);
      bytes[3U] = static_cast<uint8_t>(value >> 24U);
    };
    write_u32(header.data(), kBatchFrameMagic);
    write_u16(header.data() + 4U, kBatchFrameVersion);
    write_u16(header.data() + 6U, static_cast<uint16_t>(batch.batch_id_size));
    write_u16(header.data() + 8U, batch.record_count);
    write_u32(header.data() + 10U, static_cast<uint32_t>(batch.body_size));
    for (std::size_t index = 0U; index < batch.body_hash.size(); ++index) {
      header[14U + index] = batch.body_hash[index];
    }
    const auto write = [&](const uint8_t* bytes, std::size_t size) noexcept {
      return file.write(bytes, size) == size;
    };
    const std::array<uint8_t, 4U> marker{{
        static_cast<uint8_t>(kBatchFrameCommitMarker),
        static_cast<uint8_t>(kBatchFrameCommitMarker >> 8U),
        static_cast<uint8_t>(kBatchFrameCommitMarker >> 16U),
        static_cast<uint8_t>(kBatchFrameCommitMarker >> 24U)}};
    const bool ok = write(header.data(), header.size()) &&
                    write(reinterpret_cast<const uint8_t*>(batch.id().data()),
                          batch.batch_id_size) &&
                    write(batch.body.data(), batch.body_size) &&
                    write(marker.data(), marker.size());
    file.flush();
    file.close();
    if (!ok) return BatchStoreStatus::Retryable;
    pending_bytes_ += total;
    return BatchStoreStatus::Durable;
#else
    (void)batch;
    return BatchStoreStatus::Retryable;
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
      std::array<uint8_t, kBatchFrameHeaderBytes> header{};
      if (file.read(header.data(), header.size()) != static_cast<int>(header.size()) ||
          read_u32(header.data()) != kBatchFrameMagic ||
          read_u16(header.data() + 4U) != kBatchFrameVersion) break;
      const std::size_t id_size = read_u16(header.data() + 6U);
      const std::size_t body_size = read_u32(header.data() + 10U);
      if (id_size == 0U || id_size >= kMaxBatchIdBytes ||
          body_size == 0U || body_size > kMaxBatchBytes) break;
      std::array<uint8_t, kMaxBatchIdBytes> id{};
      if (file.read(id.data(), id_size) != static_cast<int>(id_size)) break;
      telemetry_contract::Sha256 digest{};
      std::array<uint8_t, 256U> chunk{};
      std::size_t remaining = body_size;
      bool body_read = true;
      while (remaining != 0U) {
        const std::size_t chunk_size = remaining < chunk.size() ? remaining : chunk.size();
        if (file.read(chunk.data(), chunk_size) != static_cast<int>(chunk_size)) {
          body_read = false;
          break;
        }
        digest.update(std::string_view(reinterpret_cast<const char*>(chunk.data()),
                                       chunk_size));
        remaining -= chunk_size;
      }
      std::array<uint8_t, 4U> marker{};
      if (!body_read ||
          file.read(marker.data(), marker.size()) != static_cast<int>(marker.size()) ||
          read_u32(marker.data()) != kBatchFrameCommitMarker ||
          digest.final() != read_hash(header.data() + 14U)) break;
      pending_bytes_ += kBatchFrameHeaderBytes + id_size + body_size +
                        kBatchFrameTrailerBytes;
    }
    file.close();
    return true;
#else
    return false;
#endif
  }
  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t quota_bytes() const noexcept override { return quota_bytes_; }

 private:
#ifdef ARDUINO
  static void put_u16(uint8_t* bytes, uint16_t value) noexcept {
    bytes[0U] = static_cast<uint8_t>(value);
    bytes[1U] = static_cast<uint8_t>(value >> 8U);
  }
  static void put_u32(uint8_t* bytes, uint32_t value) noexcept {
    bytes[0U] = static_cast<uint8_t>(value);
    bytes[1U] = static_cast<uint8_t>(value >> 8U);
    bytes[2U] = static_cast<uint8_t>(value >> 16U);
    bytes[3U] = static_cast<uint8_t>(value >> 24U);
  }
  static uint16_t read_u16(const uint8_t* bytes) noexcept {
    return static_cast<uint16_t>(bytes[0U] | (bytes[1U] << 8U));
  }
  static uint32_t read_u32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0U]) |
           (static_cast<uint32_t>(bytes[1U]) << 8U) |
           (static_cast<uint32_t>(bytes[2U]) << 16U) |
           (static_cast<uint32_t>(bytes[3U]) << 24U);
  }
  static config::ConfigHash read_hash(const uint8_t* bytes) noexcept {
    config::ConfigHash hash{};
    for (std::size_t index = 0U; index < hash.size(); ++index) hash[index] = bytes[index];
    return hash;
  }
  fs::FS* filesystem_{nullptr};
  const char* path_{nullptr};
#endif
  uint64_t quota_bytes_{0U};
  uint64_t pending_bytes_{0U};
};

static_assert(kBatchFrameHeaderBytes + kBatchFrameTrailerBytes <
                  kMaxBatchBytes + 128U,
              "batch frame must remain bounded");

}  // namespace sigap::telemetry_archive
