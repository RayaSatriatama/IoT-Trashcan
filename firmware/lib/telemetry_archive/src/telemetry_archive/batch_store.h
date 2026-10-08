#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "immutable_batch.h"
#include "storage/archive_codec.h"

#ifdef ARDUINO
#include <FS.h>
#endif

namespace sigap::telemetry_archive {

inline constexpr uint32_t kBatchFrameMagic{0x53474254U};
// Version 2 adds the durable source binding needed for exact ACK replay.
// Version-1 Task 6 frames are rejected because they cannot authorize reclaim.
inline constexpr uint16_t kBatchFrameVersion{2U};
inline constexpr uint32_t kBatchFrameCommitMarker{0xBA7C0EDU};
inline constexpr std::size_t kBatchFrameDeviceIdOffset{14U};
inline constexpr std::size_t kBatchFrameHashOffset{30U};
inline constexpr std::size_t kBatchFrameSourcePresentOffset{62U};
inline constexpr std::size_t kBatchFrameSourceKindOffset{63U};
inline constexpr std::size_t kBatchFrameSourceBootOffset{64U};
inline constexpr std::size_t kBatchFrameSourceSequenceOffset{80U};
inline constexpr std::size_t kBatchFrameSourceHashOffset{88U};
inline constexpr std::size_t kBatchFrameHeaderChecksumOffset{120U};
inline constexpr std::size_t kBatchFrameHeaderBytes{124U};
inline constexpr std::size_t kBatchFrameTrailerBytes{4U};
inline constexpr std::size_t kBatchIndexCapacity{64U};
inline constexpr uint32_t kBatchRecoveryMagic{0x53474252U};
inline constexpr uint8_t kBatchRecoveryVersion{1U};
inline constexpr uint32_t kBatchRecoveryMarker{0x42544352U};
inline constexpr std::size_t kBatchRecoveryFrameBytes{24U};

enum class BatchStoreStatus : uint8_t { Durable, Retryable, LoggingBlocked, Invalid };
enum class BatchStoreFault : uint8_t { None, ShortWrite, FlushFailure, CorruptTail };

class IImmutableBatchStore {
 public:
  virtual ~IImmutableBatchStore() = default;
  virtual BatchStoreStatus append(const ImmutableBatch& batch) noexcept = 0;
  virtual bool checkpoint() noexcept = 0;
  virtual bool recover() noexcept = 0;
  virtual bool load_manifest(const Ack& ack, ImmutableBatch& output) noexcept = 0;
  virtual uint64_t pending_bytes() const noexcept = 0;
  virtual uint64_t quota_bytes() const noexcept = 0;
};

template <std::size_t Capacity>
class MemoryImmutableBatchStore final : public IImmutableBatchStore {
 public:
  explicit MemoryImmutableBatchStore(uint64_t quota_bytes) noexcept
      : quota_bytes_(quota_bytes) {}

  BatchStoreStatus append(const ImmutableBatch& batch) noexcept override {
    if (!batch.attempted || !batch.device_id.present ||
        batch.record_count == 0U || batch.record_count > kMaxBatchRecords ||
        !batch.body_hash_matches()) return BatchStoreStatus::Invalid;
    if (fault_ == BatchStoreFault::ShortWrite) return BatchStoreStatus::Retryable;
    if (pending_bytes_ > quota_bytes_ ||
        batch.body_size > quota_bytes_ - pending_bytes_ || count_ == Capacity) {
      return BatchStoreStatus::LoggingBlocked;
    }
    for (std::size_t index = 0U; index < count_; ++index) {
      if (entries_[index].batch.id() == batch.id()) {
        return entries_[index].batch.body_hash == batch.body_hash &&
                       same_source(entries_[index].batch.source, batch.source)
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
  bool load(std::size_t index, ImmutableBatch& output) const noexcept {
    const auto* entry = at(index);
    if (entry == nullptr) return false;
    output = *entry;
    return true;
  }
  bool load_manifest(const Ack& ack, ImmutableBatch& output) noexcept override {
    for (std::size_t index = 0U; index < count_; ++index) {
      if (ack_matches(entries_[index].batch, ack)) {
        output = entries_[index].batch;
        return true;
      }
    }
    return false;
  }

 private:
  static bool same_source(const BatchSourceBinding& left,
                          const BatchSourceBinding& right) noexcept {
    return left.present == right.present && left.device_id.bytes == right.device_id.bytes &&
           left.boot_id.bytes == right.boot_id.bytes && left.sequence == right.sequence &&
           left.record_kind == right.record_kind && left.record_hash == right.record_hash;
  }
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
    if (filesystem_ == nullptr || !batch.attempted || !batch.device_id.present ||
        batch.record_count == 0U || batch.record_count > kMaxBatchRecords ||
        batch.batch_id_size == 0U || batch.batch_id_size >= kMaxBatchIdBytes ||
        !batch.body_hash_matches()) {
      return BatchStoreStatus::Invalid;
    }
    const auto* existing = find_index(batch);
    if (existing != nullptr) {
      return existing->body_hash == batch.body_hash &&
                     existing->device_id.bytes == batch.device_id.bytes
                 ? BatchStoreStatus::Durable
                 : BatchStoreStatus::Invalid;
    }
    // The RAM index is only a bounded cache. Once full, scan the committed
    // file so a duplicate/conflict is still detected without limiting the
    // number of durable batches to the cache size.
    if (existing == nullptr && index_count_ == index_.size()) {
      BatchIndexEntry scanned{};
      bool found{false};
      if (!scan_identity(batch, scanned, found)) return BatchStoreStatus::Retryable;
      if (found) {
        return scanned.body_hash == batch.body_hash &&
                       scanned.device_id.bytes == batch.device_id.bytes
                   ? BatchStoreStatus::Durable
                   : BatchStoreStatus::Invalid;
      }
    }
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
    for (std::size_t index = 0U; index < batch.device_id.bytes.size(); ++index) {
      header[kBatchFrameDeviceIdOffset + index] = batch.device_id.bytes[index];
    }
    for (std::size_t index = 0U; index < batch.body_hash.size(); ++index) {
      header[kBatchFrameHashOffset + index] = batch.body_hash[index];
    }
    header[kBatchFrameSourcePresentOffset] = batch.source.present ? 1U : 0U;
    if (batch.source.present) {
      if (batch.source.device_id.bytes != batch.device_id.bytes ||
          batch.source.record_kind <
              static_cast<uint8_t>(storage::ArchiveRecordKind::Sample) ||
          batch.source.record_kind >
              static_cast<uint8_t>(storage::ArchiveRecordKind::Metadata)) {
        file.close();
        return BatchStoreStatus::Invalid;
      }
      header[kBatchFrameSourceKindOffset] = batch.source.record_kind;
      std::copy(batch.source.boot_id.bytes.begin(), batch.source.boot_id.bytes.end(),
                header.begin() + kBatchFrameSourceBootOffset);
      put_u64(header.data() + kBatchFrameSourceSequenceOffset,
              batch.source.sequence);
      std::copy(batch.source.record_hash.begin(), batch.source.record_hash.end(),
                header.begin() + kBatchFrameSourceHashOffset);
    }
    put_u32(header.data() + kBatchFrameHeaderChecksumOffset,
            storage::archive_crc32(header.data(),
                                   kBatchFrameHeaderChecksumOffset));
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
    add_index(batch);
    pending_bytes_ += total;
    return BatchStoreStatus::Durable;
#else
    (void)batch;
    return BatchStoreStatus::Retryable;
#endif
  }
  bool checkpoint() noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr) return false;
    File marker = filesystem_->open(checkpoint_path_, FILE_WRITE);
    if (!marker) return false;
    std::array<uint8_t, 12U> bytes{};
    put_u32(bytes.data(), kBatchFrameCommitMarker);
    put_u64(bytes.data() + 4U, pending_bytes_);
    const bool written = marker.write(bytes.data(), bytes.size()) == bytes.size();
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
    bool marked_recovery{false};
    if (!recover_marked_tail(marked_recovery)) return false;
    if (marked_recovery) return recover();
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) {
      if (filesystem_->exists(path_)) return false;
      // Unmarked quarantine is forensic data only. A prior reclaim or an
      // interrupted older generation must never be resurrected implicitly.
      char quarantine[64]{};
      if (!make_quarantine_path(quarantine, sizeof(quarantine))) return false;
      return !filesystem_->exists(quarantine);
    }
    std::size_t valid_bytes = 0U;
    bool clean = true;
    while (file.available()) {
      std::array<uint8_t, kBatchFrameHeaderBytes> header{};
      if (file.read(header.data(), header.size()) != static_cast<int>(header.size()) ||
          !valid_header(header)) {
        clean = false;
        break;
      }
      const std::size_t id_size = read_u16(header.data() + 6U);
      const std::size_t body_size = read_u32(header.data() + 10U);
      const std::size_t record_count = read_u16(header.data() + 8U);
      if (id_size == 0U || id_size >= kMaxBatchIdBytes ||
          record_count == 0U || record_count > kMaxBatchRecords ||
          body_size == 0U || body_size > kMaxBatchBytes) {
        clean = false;
        break;
      }
      std::array<uint8_t, kMaxBatchIdBytes> id{};
      if (file.read(id.data(), id_size) != static_cast<int>(id_size)) {
        clean = false;
        break;
      }
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
      const auto body_hash = digest.final();
      if (!body_read ||
          file.read(marker.data(), marker.size()) != static_cast<int>(marker.size()) ||
          read_u32(marker.data()) != kBatchFrameCommitMarker ||
          body_hash != read_hash(header.data() + kBatchFrameHashOffset)) {
        clean = false;
        break;
      }
      BatchIndexEntry entry{};
      entry.batch_id_size = id_size;
      std::copy(id.begin(), id.begin() + id_size, entry.batch_id.begin());
      entry.device_id.present = true;
      std::copy(header.begin() + kBatchFrameDeviceIdOffset,
                header.begin() + kBatchFrameDeviceIdOffset +
                    entry.device_id.bytes.size(),
                entry.device_id.bytes.begin());
      BatchSourceBinding recovered_source{};
      if (!read_source(header, entry.device_id, recovered_source)) {
        clean = false;
        break;
      }
      entry.body_hash = body_hash;
      entry.record_count = read_u16(header.data() + 8U);
      entry.body_size = body_size;
      entry.present = true;
      // The index is a hot cache, not the archive capacity. Older entries
      // remain file-backed and are found by scan_identity/load after restart.
      if (index_count_ < index_.size()) {
        index_[index_count_++] = entry;
      }
      pending_bytes_ += kBatchFrameHeaderBytes + id_size + body_size +
                        kBatchFrameTrailerBytes;
      valid_bytes += kBatchFrameHeaderBytes + id_size + body_size +
                     kBatchFrameTrailerBytes;
    }
    file.close();
    return clean || quarantine_tail(valid_bytes);
#else
    return false;
#endif
  }
  uint64_t pending_bytes() const noexcept override { return pending_bytes_; }
  uint64_t quota_bytes() const noexcept override { return quota_bytes_; }

#ifndef ARDUINO
  bool load_manifest(const Ack&, ImmutableBatch&) noexcept override {
    return false;
  }
#endif
#ifdef ARDUINO
  // Replays one exact immutable batch after a runtime object is recreated.
  // The scan is bounded by the 32 KiB body contract and never changes bytes.
  bool load(std::size_t index, ImmutableBatch& output) noexcept {
    return load_selected(index, nullptr, output);
  }
  bool load_manifest(const Ack& ack, ImmutableBatch& output) noexcept override {
    return load_selected(0U, &ack, output);
  }

 private:
  bool load_selected(std::size_t selected_index, const Ack* ack,
                     ImmutableBatch& output) noexcept {
    if (filesystem_ == nullptr) return false;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) return false;
    std::size_t current = 0U;
    while (file.available()) {
      std::array<uint8_t, kBatchFrameHeaderBytes> header{};
      if (file.read(header.data(), header.size()) != static_cast<int>(header.size()) ||
          !valid_header(header)) {
        file.close();
        return false;
      }
      const std::size_t id_size = read_u16(header.data() + 6U);
      const std::size_t body_size = read_u32(header.data() + 10U);
      if (id_size == 0U || id_size >= kMaxBatchIdBytes ||
          body_size == 0U || body_size > kMaxBatchBytes) {
        file.close();
        return false;
      }
      std::array<uint8_t, kMaxBatchIdBytes> id{};
      if (file.read(id.data(), id_size) != static_cast<int>(id_size)) {
        file.close();
        return false;
      }
      const bool selected = ack == nullptr
                                ? current == selected_index
                                : header_matches_ack(header, id.data(), id_size,
                                                     *ack);
      ImmutableBatch& candidate = output;
      if (selected) {
        candidate = ImmutableBatch{};
        std::copy(id.begin(), id.begin() + id_size, candidate.batch_id.begin());
        candidate.batch_id_size = id_size;
        candidate.record_count = read_u16(header.data() + 8U);
        candidate.device_id.present = true;
        std::copy(header.begin() + kBatchFrameDeviceIdOffset,
                  header.begin() + kBatchFrameDeviceIdOffset +
                      candidate.device_id.bytes.size(),
                  candidate.device_id.bytes.begin());
        if (!read_source(header, candidate.device_id, candidate.source)) {
          file.close();
          return false;
        }
      }
      telemetry_contract::Sha256 digest{};
      std::array<uint8_t, 256U> chunk{};
      std::size_t remaining = body_size;
      std::size_t copied = 0U;
      while (remaining != 0U) {
        const std::size_t chunk_size = remaining < chunk.size() ? remaining : chunk.size();
        if (file.read(chunk.data(), chunk_size) != static_cast<int>(chunk_size)) {
          file.close();
          return false;
        }
        digest.update(std::string_view(reinterpret_cast<const char*>(chunk.data()),
                                       chunk_size));
        if (selected) {
          std::copy(chunk.begin(), chunk.begin() + chunk_size,
                    candidate.body.begin() + copied);
          copied += chunk_size;
        }
        remaining -= chunk_size;
      }
      std::array<uint8_t, kBatchFrameTrailerBytes> marker{};
      const auto body_hash = digest.final();
      if (file.read(marker.data(), marker.size()) != static_cast<int>(marker.size()) ||
          read_u32(marker.data()) != kBatchFrameCommitMarker ||
          body_hash != read_hash(header.data() + kBatchFrameHashOffset)) {
        file.close();
        return false;
      }
      if (selected) {
        candidate.body_size = body_size;
        candidate.body_hash = body_hash;
        candidate.attempted = true;
        output = candidate;
        file.close();
        return true;
      }
      ++current;
    }
    file.close();
    return false;
  }
#endif
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
  static uint64_t read_u64(const uint8_t* bytes) noexcept {
    uint64_t value{0U};
    for (std::size_t index = 0U; index < 8U; ++index) {
      value |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
  }
  static bool valid_header(
      const std::array<uint8_t, kBatchFrameHeaderBytes>& header) noexcept {
    return read_u32(header.data()) == kBatchFrameMagic &&
           read_u16(header.data() + 4U) == kBatchFrameVersion &&
           read_u32(header.data() + kBatchFrameHeaderChecksumOffset) ==
               storage::archive_crc32(header.data(),
                                      kBatchFrameHeaderChecksumOffset);
  }
  static config::ConfigHash read_hash(const uint8_t* bytes) noexcept {
    config::ConfigHash hash{};
    for (std::size_t index = 0U; index < hash.size(); ++index) hash[index] = bytes[index];
    return hash;
  }
  struct BatchIndexEntry final {
    std::array<char, kMaxBatchIdBytes> batch_id{};
    std::size_t batch_id_size{0U};
    acquisition::DeviceId device_id{};
    config::ConfigHash body_hash{};
    uint16_t record_count{0U};
    std::size_t body_size{0U};
    bool present{false};
  };
  static bool read_source(
      const std::array<uint8_t, kBatchFrameHeaderBytes>& header,
      const acquisition::DeviceId& device,
      BatchSourceBinding& source) noexcept {
    source = BatchSourceBinding{};
    const uint8_t present = header[kBatchFrameSourcePresentOffset];
    if (present == 0U) return true;
    if (present != 1U) return false;
    const uint8_t kind = header[kBatchFrameSourceKindOffset];
    if (kind < static_cast<uint8_t>(storage::ArchiveRecordKind::Sample) ||
        kind > static_cast<uint8_t>(storage::ArchiveRecordKind::Metadata)) {
      return false;
    }
    source.present = true;
    source.device_id = device;
    source.boot_id.present = true;
    std::copy(header.begin() + kBatchFrameSourceBootOffset,
              header.begin() + kBatchFrameSourceBootOffset +
                  source.boot_id.bytes.size(),
              source.boot_id.bytes.begin());
    source.sequence = read_u64(header.data() + kBatchFrameSourceSequenceOffset);
    source.record_kind = kind;
    std::copy(header.begin() + kBatchFrameSourceHashOffset,
              header.begin() + kBatchFrameSourceHashOffset +
                  source.record_hash.size(),
              source.record_hash.begin());
    return true;
  }
  static bool header_matches_ack(
      const std::array<uint8_t, kBatchFrameHeaderBytes>& header,
      const uint8_t* id, std::size_t id_size, const Ack& ack) noexcept {
    if (id_size != ack.batch_id_size ||
        read_u16(header.data() + 8U) != ack.record_count ||
        read_hash(header.data() + kBatchFrameHashOffset) != ack.body_hash ||
        !std::equal(id, id + id_size, ack.batch_id.begin())) {
      return false;
    }
    return std::equal(header.begin() + kBatchFrameDeviceIdOffset,
                      header.begin() + kBatchFrameDeviceIdOffset +
                          ack.device_id.bytes.size(),
                      ack.device_id.bytes.begin());
  }
  const BatchIndexEntry* find_index(const ImmutableBatch& batch) const noexcept {
    for (std::size_t index = 0U; index < index_count_; ++index) {
      const auto& entry = index_[index];
      if (entry.present && entry.batch_id_size == batch.batch_id_size &&
          entry.device_id.bytes == batch.device_id.bytes &&
          std::equal(entry.batch_id.begin(),
                     entry.batch_id.begin() + entry.batch_id_size,
                     batch.batch_id.begin())) {
        return &entry;
      }
    }
    return nullptr;
  }
  bool scan_identity(const ImmutableBatch& batch,
                     BatchIndexEntry& match,
                     bool& found) const noexcept {
    found = false;
    if (filesystem_ == nullptr) return false;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) return !filesystem_->exists(path_);
    while (file.available()) {
      std::array<uint8_t, kBatchFrameHeaderBytes> header{};
      if (file.read(header.data(), header.size()) !=
              static_cast<int>(header.size()) ||
          !valid_header(header)) {
        file.close();
        return false;
      }
      const std::size_t id_size = read_u16(header.data() + 6U);
      const std::size_t body_size = read_u32(header.data() + 10U);
      const std::size_t record_count = read_u16(header.data() + 8U);
      if (id_size == 0U || id_size >= kMaxBatchIdBytes ||
          body_size == 0U || body_size > kMaxBatchBytes ||
          record_count == 0U || record_count > kMaxBatchRecords) {
        file.close();
        return false;
      }
      std::array<char, kMaxBatchIdBytes> id{};
      if (file.read(reinterpret_cast<uint8_t*>(id.data()), id_size) !=
          static_cast<int>(id_size)) {
        file.close();
        return false;
      }
      telemetry_contract::Sha256 digest{};
      std::array<uint8_t, 256U> chunk{};
      std::size_t remaining = body_size;
      while (remaining != 0U) {
        const std::size_t chunk_size = remaining < chunk.size() ? remaining : chunk.size();
        if (file.read(chunk.data(), chunk_size) != static_cast<int>(chunk_size)) {
          file.close();
          return false;
        }
        digest.update(std::string_view(reinterpret_cast<const char*>(chunk.data()),
                                       chunk_size));
        remaining -= chunk_size;
      }
      std::array<uint8_t, kBatchFrameTrailerBytes> marker{};
      const auto body_hash = digest.final();
      if (file.read(marker.data(), marker.size()) !=
              static_cast<int>(marker.size()) ||
          read_u32(marker.data()) != kBatchFrameCommitMarker ||
          body_hash != read_hash(header.data() + kBatchFrameHashOffset)) {
        file.close();
        return false;
      }
      const bool same_id = id_size == batch.batch_id_size &&
                           std::equal(id.begin(), id.begin() + id_size,
                                      batch.batch_id.begin());
      bool same_device = true;
      for (std::size_t index = 0U; index < batch.device_id.bytes.size(); ++index) {
        if (header[kBatchFrameDeviceIdOffset + index] !=
            batch.device_id.bytes[index]) {
          same_device = false;
          break;
        }
      }
      if (same_id && same_device) {
        match.batch_id_size = id_size;
        std::copy(id.begin(), id.begin() + id_size, match.batch_id.begin());
        match.device_id = batch.device_id;
        match.body_hash = body_hash;
        match.record_count = static_cast<uint16_t>(record_count);
        match.body_size = body_size;
        BatchSourceBinding recovered_source{};
        if (!read_source(header, match.device_id, recovered_source)) {
          file.close();
          return false;
        }
        match.present = true;
        found = true;
        file.close();
        return true;
      }
    }
    file.close();
    return true;
  }
  void add_index(const ImmutableBatch& batch) noexcept {
    if (index_count_ >= index_.size()) return;
    auto& entry = index_[index_count_++];
    entry.batch_id_size = batch.batch_id_size;
    std::copy(batch.batch_id.begin(), batch.batch_id.begin() + batch.batch_id_size,
              entry.batch_id.begin());
    entry.device_id = batch.device_id;
    entry.body_hash = batch.body_hash;
    entry.record_count = batch.record_count;
    entry.body_size = batch.body_size;
    entry.present = true;
  }
  static void put_u64(uint8_t* bytes, uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
      bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
  }
  bool make_quarantine_path(char* output, std::size_t size) const noexcept {
    if (output == nullptr || path_ == nullptr) return false;
    const int written = std::snprintf(output, size, "%s.tail", path_);
    return written > 0 && static_cast<std::size_t>(written) < size;
  }
  bool recovery_marker_path(char* output, std::size_t size) const noexcept {
    if (output == nullptr || path_ == nullptr) return false;
    const int written = std::snprintf(output, size, "%s.recover", path_);
    return written > 0 && static_cast<std::size_t>(written) < size;
  }
  bool write_recovery_marker(std::size_t valid_bytes) noexcept {
    char marker_path[64]{};
    if (filesystem_ == nullptr ||
        !recovery_marker_path(marker_path, sizeof(marker_path))) return false;
    std::array<uint8_t, kBatchRecoveryFrameBytes> frame{};
    put_u32(frame.data(), kBatchRecoveryMagic);
    frame[4U] = kBatchRecoveryVersion;
    put_u64(frame.data() + 6U, static_cast<uint64_t>(valid_bytes));
    put_u32(frame.data() + 14U,
            storage::archive_crc32(frame.data(), 14U));
    put_u32(frame.data() + 18U, kBatchRecoveryMarker);
    File marker = filesystem_->open(marker_path, FILE_WRITE);
    if (!marker) return false;
    const bool written = marker.write(frame.data(), frame.size()) ==
                         static_cast<int>(frame.size());
    if (written) marker.flush();
    marker.close();
    return written;
  }
  bool copy_quarantine_prefix(std::size_t valid_bytes) noexcept {
    char quarantine[64]{};
    if (filesystem_ == nullptr ||
        !make_quarantine_path(quarantine, sizeof(quarantine)) ||
        !filesystem_->exists(quarantine)) return false;
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
    return copied && remaining == 0U &&
           destination_size_matches(path_, valid_bytes);
  }
  bool recover_marked_tail(bool& marker_found) noexcept {
    marker_found = false;
    char marker_path[64]{};
    if (filesystem_ == nullptr ||
        !recovery_marker_path(marker_path, sizeof(marker_path))) return false;
    File marker = filesystem_->open(marker_path, FILE_READ);
    if (!marker) return true;
    marker_found = true;
    if (marker.size() != kBatchRecoveryFrameBytes) {
      marker.close();
      return false;
    }
    std::array<uint8_t, kBatchRecoveryFrameBytes> frame{};
    const bool read_ok = marker.read(frame.data(), frame.size()) ==
                         static_cast<int>(frame.size());
    marker.close();
    if (!read_ok || read_u32(frame.data()) != kBatchRecoveryMagic ||
        frame[4U] != kBatchRecoveryVersion ||
        read_u32(frame.data() + 14U) !=
            storage::archive_crc32(frame.data(), 14U) ||
        read_u32(frame.data() + 18U) != kBatchRecoveryMarker) return false;
    const uint64_t encoded = read_u64(frame.data() + 6U);
    if (encoded > static_cast<uint64_t>(SIZE_MAX) ||
        !copy_quarantine_prefix(static_cast<std::size_t>(encoded))) return false;
    return filesystem_->remove(marker_path);
  }
  bool quarantine_tail(std::size_t valid_bytes) noexcept {
    if (filesystem_ == nullptr) return false;
    char quarantine[64]{};
    char marker_path[64]{};
    const int written = std::snprintf(quarantine, sizeof(quarantine),
                                      "%s.tail", path_);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(quarantine) ||
        !recovery_marker_path(marker_path, sizeof(marker_path))) {
      return false;
    }
    if (filesystem_->exists(quarantine)) return false;
    if (!filesystem_->rename(path_, quarantine)) return false;
    if (!write_recovery_marker(valid_bytes)) {
      (void)filesystem_->rename(quarantine, path_);
      return false;
    }
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
    if (!copied || remaining != 0U ||
        !destination_size_matches(path_, valid_bytes)) {
      return false;
    }
    return filesystem_->remove(marker_path);
  }
  bool destination_size_matches(const char* path,
                                std::size_t expected) noexcept {
    if (filesystem_ == nullptr) return false;
    File file = filesystem_->open(path, FILE_READ);
    if (!file) return false;
    const bool matches = static_cast<std::size_t>(file.size()) == expected;
    file.close();
    return matches;
  }
  fs::FS* filesystem_{nullptr};
  const char* path_{nullptr};
  const char* checkpoint_path_{"/sigap/batches.ckp"};
  std::array<BatchIndexEntry, kBatchIndexCapacity> index_{};
  std::size_t index_count_{0U};
#endif
  uint64_t quota_bytes_{0U};
  uint64_t pending_bytes_{0U};
};

static_assert(kBatchFrameHeaderBytes + kBatchFrameTrailerBytes <
                  kMaxBatchBytes + 128U,
              "batch frame must remain bounded");
static_assert(kBatchFrameDeviceIdOffset + 16U == kBatchFrameHashOffset,
              "batch identity layout must remain contiguous");
static_assert(kBatchFrameHashOffset + 32U == kBatchFrameSourcePresentOffset,
              "batch body identity layout must remain contiguous");
static_assert(kBatchFrameSourceHashOffset + 32U ==
                  kBatchFrameHeaderChecksumOffset,
              "batch source binding layout must remain contiguous");
static_assert(kBatchFrameHeaderChecksumOffset + sizeof(uint32_t) ==
                  kBatchFrameHeaderBytes,
              "batch header checksum layout must remain exact");

}  // namespace sigap::telemetry_archive
