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
  static constexpr std::size_t kGapFrameBytes{88U};
  static constexpr std::size_t kGapReserveBytes{4096U};

  explicit LittleFsArchiveStore(const char* path = "/sigap/archive.bin") noexcept
      : path_(path) {}

#ifdef ARDUINO
  bool begin(fs::FS& filesystem, uint64_t quota_bytes) noexcept {
    if (filesystem_ == &filesystem && quota_bytes_ == quota_bytes &&
        recovery_ready_) {
      return true;
    }
    filesystem_ = &filesystem;
    quota_bytes_ = quota_bytes;
    if (!filesystem_->exists("/sigap") && !filesystem_->mkdir("/sigap")) {
      filesystem_ = nullptr;
      health_ = ArchiveHealth::Degraded;
      return false;
    }
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
    if (filesystem_ == nullptr || !recovery_ready_ || !RecordCodec::valid(record)) {
      return AppendStatus::Retryable;
    }
    const auto* existing = find_index(record.identity);
    if (existing != nullptr) {
      return *existing == record.hash ? AppendStatus::Durable
                                      : AppendStatus::Invalid;
    }
    const bool same_boot_sequence =
        have_last_identity_ &&
        record.identity.device_id.bytes == last_identity_.device_id.bytes &&
        record.identity.boot_id.bytes == last_identity_.boot_id.bytes &&
        record.identity.sequence > last_identity_.sequence;
    if (!same_boot_sequence) {
      bool disk_read_ok = true;
      config::ConfigHash disk_hash{};
      if (find_on_disk(record.identity, SIZE_MAX, disk_hash, disk_read_ok)) {
        return disk_hash == record.hash ? AppendStatus::Durable
                                        : AppendStatus::Invalid;
      }
      if (!disk_read_ok) {
        health_ = ArchiveHealth::Degraded;
        return AppendStatus::Retryable;
      }
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
    if (index_count_ < index_.size()) {
      index_[index_count_] = record.identity;
      hashes_[index_count_] = record.hash;
      ++index_count_;
    }
    pending_bytes_ += record.size;
    last_identity_ = record.identity;
    have_last_identity_ = true;
    health_ = ArchiveHealth::Healthy;
    return AppendStatus::Durable;
#else
    (void)record;
    return AppendStatus::Retryable;
#endif
  }

  // The maintenance SD probe reuses this instance-owned codec workspace so
  // its archive verification cannot place a 16 KiB frame on loopTask's stack.
  AppendStatus append_probe_record(uint64_t monotonic_us,
                                   ArchiveIdentity& identity,
                                   config::ConfigHash& hash) noexcept {
#ifdef ARDUINO
    probe_sample_ = acquisition::SampleRecord{};
    probe_sample_.header.device_id.present = true;
    probe_sample_.header.device_id.bytes[0U] = 0x53U;
    probe_sample_.header.boot_id.present = true;
    probe_sample_.header.boot_id.bytes[0U] = 0x44U;
    // A maintenance probe is a distinct synthetic metadata attempt on every
    // invocation. Use the observed monotonic time as its bounded identity so
    // an interrupted prior probe cannot turn a later verification into a
    // same-identity hash conflict.
    probe_sample_.header.sequence = monotonic_us == 0U
                                        ? 0x53444750524F4245ULL
                                        : monotonic_us;
    probe_sample_.header.monotonic_us = monotonic_us;
    probe_sample_.header.sensor = acquisition::SensorId::Mq4;
    probe_sample_.header.stream = acquisition::StreamId::Organic;
    probe_sample_.header.kind = acquisition::SampleKind::MqAdc;
    probe_sample_.quality = acquisition::SampleQuality::Timeout;
    probe_sample_.reason = acquisition::SampleReason::Timeout;
    if (!RecordCodec::encode_sample(probe_sample_, scan_record_)) {
      return AppendStatus::Invalid;
    }
    identity = scan_record_.identity;
    hash = scan_record_.hash;
    return append(scan_record_);
#else
    (void)monotonic_us;
    (void)identity;
    (void)hash;
    return AppendStatus::Retryable;
#endif
  }

#ifdef ARDUINO
  bool verify_probe_file(fs::FS& filesystem, const char* path,
                         const uint8_t* expected, std::size_t size,
                         config::ConfigHash& actual_hash) noexcept {
    actual_hash.fill(0U);
    if (path == nullptr || expected == nullptr || size == 0U ||
        size > copy_buffer_.size()) return false;
    File input = filesystem.open(path, FILE_READ);
    if (!input || input.size() != size) {
      if (input) input.close();
      return false;
    }
    const bool read = input.read(copy_buffer_.data(), size) ==
                      static_cast<int>(size);
    input.close();
    if (!read || !std::equal(copy_buffer_.begin(), copy_buffer_.begin() + size,
                             expected)) return false;
    actual_hash = telemetry_contract::sha256(std::string_view(
        reinterpret_cast<const char*>(copy_buffer_.data()), size));
    return true;
  }
#endif

  AppendStatus append_gap(const GapRange& range) noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr || gap_repair_pending_ || !range.present ||
        range.record_count == 0U) {
      return AppendStatus::Invalid;
    }
    if (gap_bytes_ > kGapReserveBytes ||
        kGapFrameBytes > kGapReserveBytes - gap_bytes_) {
      health_ = ArchiveHealth::Blocked;
      return AppendStatus::LoggingBlocked;
    }
    File file = filesystem_->open(gap_path_, FILE_APPEND);
    if (!file) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    std::array<uint8_t, kGapFrameBytes> frame{};
    write_u32(frame.data(), kGapMagic);
    frame[4U] = kGapVersion;
    frame[5U] = static_cast<uint8_t>(range.reason);
    frame[6U] = static_cast<uint8_t>(range.sensor);
    frame[7U] = static_cast<uint8_t>(range.stream);
    write_u64(frame.data() + 8U, range.first_sequence);
    write_u64(frame.data() + 16U, range.last_sequence);
    write_u64(frame.data() + 24U, range.first_monotonic_us);
    write_u64(frame.data() + 32U, range.last_monotonic_us);
    write_u64(frame.data() + 40U, range.record_count);
    std::memcpy(frame.data() + 48U, range.device_id.bytes.data(),
                range.device_id.bytes.size());
    std::memcpy(frame.data() + 64U, range.boot_id.bytes.data(),
                range.boot_id.bytes.size());
    write_u32(frame.data() + 80U, archive_crc32(frame.data(), 80U));
    write_u32(frame.data() + 84U, kGapMarker);
    const bool written = file.write(frame.data(), frame.size()) == frame.size();
    file.flush();
    file.close();
    if (!written) {
      health_ = ArchiveHealth::Degraded;
      return AppendStatus::Retryable;
    }
    gap_bytes_ += frame.size();
    return AppendStatus::Durable;
#else
    (void)range;
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
    recovery_ready_ = false;
    pending_bytes_ = 0U;
    gap_bytes_ = 0U;
    gap_repair_pending_ = false;
    have_last_identity_ = false;
    last_identity_ = ArchiveIdentity{};
    bool marked_recovery{false};
    if (!recover_marked_quarantine(marked_recovery)) {
      health_ = ArchiveHealth::Degraded;
      return false;
    }
    if (marked_recovery) return recover();
    File gap_file = filesystem_->open(gap_path_, FILE_READ);
    bool gap_valid = true;
    if (gap_file) {
      gap_valid = validate_gap_file(gap_file, gap_bytes_);
      gap_file.close();
      if (!gap_valid && !repair_gap_prefix(gap_bytes_)) {
        // Keep the corrupt source in place for forensic recovery.  Archive
        // records remain recoverable, but the reserve path is degraded until
        // the gap journal can be repaired.
        gap_bytes_ = 0U;
        gap_repair_pending_ = true;
      }
    }
    index_count_ = 0U;
    File file = filesystem_->open(path_, FILE_READ);
    if (file && file.size() == 0U && has_quarantine()) {
      // A recovery marker is handled before opening the archive. Reaching an
      // empty primary here therefore means an earlier repair completed and a
      // later legitimate reclaim removed the final pending frame. The tail
      // remains forensic-only and must not block new appends or be restored.
      file.close();
      recovery_ready_ = true;
      health_ = gap_valid ? ArchiveHealth::Healthy : ArchiveHealth::Degraded;
      return true;
    }
    if (!file) {
      if (!filesystem_->exists(path_)) {
        char backup[96]{};
        if (make_related_path(backup, sizeof(backup), ".migrate.bak") &&
            filesystem_->exists(backup)) {
          // A backup is forensic evidence after a reclaim transaction. It is
          // not an authority to resurrect a record that was already copied to
          // the destination. Only a still-pending replacement is eligible
          // for completion; otherwise leave the source degraded and let the
          // durable migration receipt decide the next action.
          char temporary[96]{};
          if (make_related_path(temporary, sizeof(temporary), ".migrate") &&
              filesystem_->exists(temporary)) {
            File replacement = filesystem_->open(temporary, FILE_READ);
            if (!replacement) {
              health_ = ArchiveHealth::Degraded;
              return false;
            }
            const std::size_t replacement_bytes =
                static_cast<std::size_t>(replacement.size());
            replacement.close();
            if (!verify_valid_prefix(temporary, replacement_bytes) ||
                !filesystem_->rename(temporary, path_)) {
              health_ = ArchiveHealth::Degraded;
              return false;
            }
            return recover();
          }
          health_ = ArchiveHealth::Degraded;
          return false;
        }
        // Never choose a quarantine by size or age.  A marked recovery is
        // replayed above; an unmarked tail remains forensic-only until an
        // explicit repair path is available.
        if (has_quarantine()) {
          health_ = ArchiveHealth::Degraded;
          return false;
        }
        recovery_ready_ = true;
        health_ = gap_valid ? ArchiveHealth::Healthy : ArchiveHealth::Degraded;
        return true;
      }
      health_ = ArchiveHealth::Degraded;
      return false;
    }
    std::size_t valid_bytes = 0U;
    bool tail_corrupt = false;
    bool conflict_seen = false;
    while (file.available()) {
      ImmutableRecordBytes& record = scan_record_;
      record.bytes.fill(0U);
      record.size = 0U;
      record.identity = ArchiveIdentity{};
      record.hash.fill(0U);
      record.committed = false;
      if (file.read(record.bytes.data(), kArchiveHeaderBytes) !=
          static_cast<int>(kArchiveHeaderBytes)) {
        tail_corrupt = true;
        break;
      }
      const uint32_t payload_size = read_u32(record.bytes.data() + 8U);
      if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                             kArchiveTrailerBytes) {
        tail_corrupt = true;
        break;
      }
      record.size = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
      const std::size_t remaining = record.size - kArchiveHeaderBytes;
      if (file.read(record.bytes.data() + kArchiveHeaderBytes, remaining) !=
          static_cast<int>(remaining)) {
        tail_corrupt = true;
        break;
      }
      record.committed = true;
      if (!RecordCodec::valid(record)) {
        tail_corrupt = true;
        break;
      }
      decode_identity(record);
      record.hash = telemetry_contract::sha256(std::string_view(
          reinterpret_cast<const char*>(record.bytes.data()), record.size));
      const auto* existing = find_index(record.identity);
      if (existing != nullptr) {
        if (*existing != record.hash) conflict_seen = true;
        valid_bytes += record.size;
        continue;
      }
      // Normal production records use the shared monotonic sequence allocator
      // and therefore arrive in sequence order. Avoid an O(n^2) full-prefix
      // scan while rebuilding a large archive. Only an out-of-order sequence
      // can be a late duplicate/conflict, so use the bounded on-disk lookup
      // for that exceptional case.
      const bool same_boot_sequence =
          have_last_identity_ &&
          record.identity.device_id.bytes == last_identity_.device_id.bytes &&
          record.identity.boot_id.bytes == last_identity_.boot_id.bytes &&
          record.identity.sequence > last_identity_.sequence;
      if (!same_boot_sequence) {
        bool prior_read_ok = true;
        config::ConfigHash prior_hash{};
        if (find_on_disk(record.identity, valid_bytes, prior_hash,
                         prior_read_ok)) {
          if (prior_hash != record.hash) conflict_seen = true;
          valid_bytes += record.size;
          last_identity_ = record.identity;
          have_last_identity_ = true;
          continue;
        }
        if (!prior_read_ok) {
          tail_corrupt = true;
          break;
        }
      }
      if (index_count_ < index_.size()) {
        index_[index_count_] = record.identity;
        hashes_[index_count_] = record.hash;
        ++index_count_;
      }
      pending_bytes_ += record.size;
      valid_bytes += record.size;
      last_identity_ = record.identity;
      have_last_identity_ = true;
    }
    file.close();
    if (tail_corrupt && !quarantine_tail(valid_bytes)) {
      health_ = ArchiveHealth::Degraded;
      return false;
    }
    recovery_ready_ = true;
    health_ = (conflict_seen || !gap_valid) ? ArchiveHealth::Degraded
                                             : ArchiveHealth::Healthy;
    return true;
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

  bool peek_pending(ImmutableRecordBytes& output) noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr || !recovery_ready_) return false;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) return false;
    const bool ok = read_record(file, output);
    file.close();
    return ok;
#else
    (void)output;
    return false;
#endif
  }

  bool reclaim_pending(const ArchiveIdentity& identity,
                       const config::ConfigHash& hash) noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr || !recovery_ready_) return false;
    File source = filesystem_->open(path_, FILE_READ);
    if (!source) {
      return false;
    }
    bool found = false;
    while (source.available()) {
      if (!read_record(source, scan_record_)) {
        source.close();
        return false;
      }
      if (same_identity(scan_record_.identity, identity)) {
        if (scan_record_.hash != hash) {
          source.close();
          health_ = ArchiveHealth::Degraded;
          return false;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      if (source) source.close();
      return false;
    }
    char temporary[96]{};
    char backup[96]{};
    if (!make_related_path(temporary, sizeof(temporary), ".migrate") ||
        !make_related_path(backup, sizeof(backup), ".migrate.bak")) {
      source.close();
      return false;
    }
    if (filesystem_->exists(temporary)) (void)filesystem_->remove(temporary);
    File destination = filesystem_->open(temporary, FILE_WRITE);
    if (!destination || !source.seek(0U, SeekSet)) {
      if (destination) destination.close();
      source.close();
      return false;
    }
    bool copied = true;
    while (source.available()) {
      if (!read_record(source, scan_record_)) {
        copied = false;
        break;
      }
      if (same_identity(scan_record_.identity, identity)) continue;
      if (destination.write(scan_record_.bytes.data(), scan_record_.size) !=
          scan_record_.size) {
        copied = false;
        break;
      }
    }
    destination.flush();
    destination.close();
    source.close();
    if (!copied || !filesystem_->rename(path_, backup) ||
        !filesystem_->rename(temporary, path_)) {
      if (filesystem_->exists(temporary)) (void)filesystem_->remove(temporary);
      if (!filesystem_->exists(path_) && filesystem_->exists(backup)) {
        (void)filesystem_->rename(backup, path_);
      }
      health_ = ArchiveHealth::Degraded;
      return false;
    }
    (void)filesystem_->remove(backup);
    return recover();
#else
    (void)identity;
    (void)hash;
    return false;
#endif
  }

  bool contains(const ArchiveIdentity& identity,
                const config::ConfigHash& hash) noexcept override {
#ifdef ARDUINO
    if (filesystem_ == nullptr || !recovery_ready_) return false;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) return false;
    bool found = false;
    while (file.available()) {
      if (!read_record(file, lookup_record_)) break;
      if (same_identity(lookup_record_.identity, identity)) {
        found = lookup_record_.hash == hash;
        break;
      }
    }
    file.close();
    return found;
#else
    (void)identity;
    (void)hash;
    return false;
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
  uint64_t gap_bytes_{0U};
  bool gap_repair_pending_{false};

 private:
#ifdef ARDUINO
  static constexpr uint32_t kGapMagic{0x53474150U};
  static constexpr uint8_t kGapVersion{1U};
  static constexpr uint32_t kGapMarker{0x47415021U};
  static constexpr uint32_t kRecoveryMagic{0x53494752U};
  static constexpr uint8_t kRecoveryVersion{1U};
  static constexpr uint32_t kRecoveryMarker{0x52454356U};
  static constexpr std::size_t kRecoveryFrameBytes{24U};

  bool validate_gap_file(File& file, uint64_t& valid_bytes) noexcept {
    const std::size_t size = file.size();
    valid_bytes = 0U;
    if (size > kGapReserveBytes || (size % kGapFrameBytes) != 0U) {
      return false;
    }
    std::array<uint8_t, kGapFrameBytes> frame{};
    while (valid_bytes < size) {
      if (file.read(frame.data(), frame.size()) !=
          static_cast<int>(frame.size())) {
        return false;
      }
      if (read_u32(frame.data()) != kGapMagic ||
          frame[4U] != kGapVersion ||
          read_u32(frame.data() + 80U) != archive_crc32(frame.data(), 80U) ||
          read_u32(frame.data() + 84U) != kGapMarker ||
          read_u64(frame.data() + 40U) == 0U ||
          read_u64(frame.data() + 16U) < read_u64(frame.data() + 8U)) {
        return false;
      }
      valid_bytes += frame.size();
    }
    return true;
  }

  static bool same_identity(const ArchiveIdentity& left,
                            const ArchiveIdentity& right) noexcept {
    return left.kind == right.kind && left.sequence == right.sequence &&
           left.device_id.bytes == right.device_id.bytes &&
           left.boot_id.bytes == right.boot_id.bytes;
  }
  bool make_related_path(char* output, std::size_t size,
                         const char* suffix) const noexcept {
    if (output == nullptr || suffix == nullptr || path_ == nullptr) return false;
    const int written = std::snprintf(output, size, "%s%s", path_, suffix);
    return written > 0 && static_cast<std::size_t>(written) < size;
  }
  bool recovery_marker_path(char* output, std::size_t size) const noexcept {
    return make_related_path(output, size, ".recover");
  }
  static bool read_record(File& file, ImmutableRecordBytes& record) noexcept {
    record.bytes.fill(0U);
    record.size = 0U;
    record.identity = ArchiveIdentity{};
    record.hash.fill(0U);
    record.committed = false;
    if (file.read(record.bytes.data(), kArchiveHeaderBytes) !=
        static_cast<int>(kArchiveHeaderBytes)) return false;
    const uint32_t payload_size = read_u32(record.bytes.data() +
                                           kArchivePayloadSizeOffset);
    if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                           kArchiveTrailerBytes) return false;
    record.size = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
    if (file.read(record.bytes.data() + kArchiveHeaderBytes,
                  record.size - kArchiveHeaderBytes) !=
        static_cast<int>(record.size - kArchiveHeaderBytes)) return false;
    record.committed = true;
    if (!RecordCodec::valid(record)) return false;
    decode_identity(record);
    record.hash = telemetry_contract::sha256(std::string_view(
        reinterpret_cast<const char*>(record.bytes.data()), record.size));
    return true;
  }
  static void decode_identity(ImmutableRecordBytes& record) noexcept {
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
  }
  const config::ConfigHash* find_index(const ArchiveIdentity& identity) const noexcept {
    for (std::size_t i = 0U; i < index_count_; ++i) {
      if (same_identity(index_[i], identity)) {
        return &hashes_[i];
      }
    }
    return nullptr;
  }
  bool find_on_disk(const ArchiveIdentity& identity,
                    std::size_t limit_bytes,
                    config::ConfigHash& hash,
                    bool& read_ok) noexcept {
    read_ok = true;
    if (filesystem_ == nullptr) {
      read_ok = false;
      return false;
    }
    if (!filesystem_->exists(path_)) return false;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) {
      read_ok = false;
      return false;
    }
    std::size_t offset = 0U;
    while (file.available() && offset < limit_bytes) {
      ImmutableRecordBytes& record = lookup_record_;
      record.bytes.fill(0U);
      record.size = 0U;
      record.identity = ArchiveIdentity{};
      record.hash.fill(0U);
      record.committed = false;
      if (limit_bytes - offset < kArchiveHeaderBytes ||
          file.read(record.bytes.data(), kArchiveHeaderBytes) !=
              static_cast<int>(kArchiveHeaderBytes)) {
        read_ok = false;
        break;
      }
      const uint32_t payload_size = read_u32(
          record.bytes.data() + kArchivePayloadSizeOffset);
      if (payload_size > kArchiveMaxRecordBytes - kArchiveHeaderBytes -
                             kArchiveTrailerBytes) {
        read_ok = false;
        break;
      }
      record.size = kArchiveHeaderBytes + payload_size + kArchiveTrailerBytes;
      if (record.size > limit_bytes - offset ||
          file.read(record.bytes.data() + kArchiveHeaderBytes,
                    record.size - kArchiveHeaderBytes) !=
              static_cast<int>(record.size - kArchiveHeaderBytes)) {
        read_ok = false;
        break;
      }
      record.committed = true;
      if (!RecordCodec::valid(record)) {
        read_ok = false;
        break;
      }
      decode_identity(record);
      record.hash = telemetry_contract::sha256(std::string_view(
          reinterpret_cast<const char*>(record.bytes.data()), record.size));
      if (same_identity(record.identity, identity)) {
        hash = record.hash;
        file.close();
        return true;
      }
      offset += record.size;
    }
    file.close();
    return false;
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
  bool has_quarantine() const noexcept {
    if (filesystem_ == nullptr || path_ == nullptr) return false;
    for (unsigned suffix = 0U; suffix < 4U; ++suffix) {
      char quarantine[64]{};
      const int written = std::snprintf(quarantine, sizeof(quarantine),
                                        "%s.tail%u", path_, suffix);
      if (written > 0 && static_cast<std::size_t>(written) < sizeof(quarantine) &&
          filesystem_->exists(quarantine)) {
        return true;
      }
    }
    return false;
  }
  bool repair_gap_prefix(std::size_t valid_bytes) noexcept {
    if (filesystem_ == nullptr || gap_path_ == nullptr ||
        valid_bytes > kGapReserveBytes) return false;
    char temporary[96]{};
    if (std::snprintf(temporary, sizeof(temporary), "%s.repair", gap_path_) <= 0 ||
        filesystem_->exists(temporary) ||
        !copy_valid_prefix(gap_path_, temporary, valid_bytes)) {
      return false;
    }
    char backup[96]{};
    bool backup_available = false;
    for (unsigned suffix = 0U; suffix < 4U; ++suffix) {
      const int written = std::snprintf(backup, sizeof(backup), "%s.bak%u",
                                        gap_path_, suffix);
      if (written > 0 && static_cast<std::size_t>(written) < sizeof(backup) &&
          !filesystem_->exists(backup)) {
        backup_available = true;
        break;
      }
    }
    if (!backup_available || !filesystem_->rename(gap_path_, backup) ||
        !filesystem_->rename(temporary, gap_path_)) {
      if (filesystem_->exists(temporary)) (void)filesystem_->remove(temporary);
      if (!filesystem_->exists(gap_path_) && filesystem_->exists(backup)) {
        (void)filesystem_->rename(backup, gap_path_);
      }
      return false;
    }
    // Do not delete the backup until a later recovery has validated the new
    // prefix.  This leaves a bounded forensic copy after a short write.
    return true;
  }
  bool write_recovery_marker(uint8_t suffix,
                             std::size_t valid_bytes) noexcept {
    char marker_path[96]{};
    if (filesystem_ == nullptr || !recovery_marker_path(marker_path,
                                                         sizeof(marker_path))) {
      return false;
    }
    std::array<uint8_t, kRecoveryFrameBytes> frame{};
    write_u32(frame.data(), kRecoveryMagic);
    frame[4U] = kRecoveryVersion;
    frame[5U] = suffix;
    write_u64(frame.data() + 6U, static_cast<uint64_t>(valid_bytes));
    write_u32(frame.data() + 14U, archive_crc32(frame.data(), 14U));
    write_u32(frame.data() + 18U, kRecoveryMarker);
    File file = filesystem_->open(marker_path, FILE_WRITE);
    if (!file) return false;
    const bool written = file.write(frame.data(), frame.size()) ==
                         static_cast<int>(frame.size());
    if (written) file.flush();
    file.close();
    return written;
  }

  bool recover_marked_quarantine(bool& marker_found) noexcept {
    marker_found = false;
    char marker_path[96]{};
    if (filesystem_ == nullptr || !recovery_marker_path(marker_path,
                                                         sizeof(marker_path))) {
      return false;
    }
    File marker = filesystem_->open(marker_path, FILE_READ);
    if (!marker) return true;
    marker_found = true;
    if (marker.size() != kRecoveryFrameBytes) {
      marker.close();
      return false;
    }
    std::array<uint8_t, kRecoveryFrameBytes> frame{};
    const bool read_ok = marker.read(frame.data(), frame.size()) ==
                         static_cast<int>(frame.size());
    marker.close();
    if (!read_ok || read_u32(frame.data()) != kRecoveryMagic ||
        frame[4U] != kRecoveryVersion || frame[5U] >= 4U ||
        read_u32(frame.data() + 14U) != archive_crc32(frame.data(), 14U) ||
        read_u32(frame.data() + 18U) != kRecoveryMarker) {
      return false;
    }
    char quarantine[64]{};
    const int written = std::snprintf(quarantine, sizeof(quarantine), "%s.tail%u",
                                      path_, static_cast<unsigned>(frame[5U]));
    const std::size_t valid_bytes = static_cast<std::size_t>(
        read_u64(frame.data() + 6U));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(quarantine) ||
        !filesystem_->exists(quarantine) ||
        !copy_valid_prefix(quarantine, path_, valid_bytes) ||
        !verify_valid_prefix(path_, valid_bytes)) {
      return false;
    }
    return filesystem_->remove(marker_path);
  }

  bool verify_valid_prefix(const char* source_path,
                           std::size_t expected_bytes) noexcept {
    if (filesystem_ == nullptr || source_path == nullptr ||
        !filesystem_->exists(source_path)) return false;
    File file = filesystem_->open(source_path, FILE_READ);
    if (!file || file.size() != expected_bytes) {
      if (file) file.close();
      return false;
    }
    std::size_t measured{0U};
    while (measured < expected_bytes) {
      if (!read_record(file, lookup_record_)) {
        file.close();
        return false;
      }
      measured += lookup_record_.size;
    }
    file.close();
    return measured == expected_bytes;
  }

  bool quarantine_tail(std::size_t valid_bytes) noexcept {
    if (filesystem_ == nullptr) return false;
    char quarantine[64]{};
    if (!make_quarantine_path(quarantine, sizeof(quarantine))) return false;
    unsigned suffix{0U};
    bool suffix_found{false};
    for (; suffix < 4U; ++suffix) {
      char expected[64]{};
      const int written = std::snprintf(expected, sizeof(expected), "%s.tail%u",
                                        path_, suffix);
      if (written > 0 && static_cast<std::size_t>(written) < sizeof(expected) &&
          std::strcmp(expected, quarantine) == 0) {
        suffix_found = true;
        break;
      }
    }
    if (!suffix_found || !filesystem_->rename(path_, quarantine)) return false;
    if (!write_recovery_marker(static_cast<uint8_t>(suffix), valid_bytes)) {
      (void)filesystem_->rename(quarantine, path_);
      return false;
    }
    if (!copy_valid_prefix(quarantine, path_, valid_bytes) ||
        !verify_valid_prefix(path_, valid_bytes)) {
      return false;
    }
    // The copy is now verified and the replacement is authoritative. Keep
    // the quarantine as forensic evidence, but retire the transaction marker
    // before accepting new appends so a later restart cannot resurrect a
    // legitimately reclaimed prefix.
    char marker_path[96]{};
    return recovery_marker_path(marker_path, sizeof(marker_path)) &&
           filesystem_->remove(marker_path);
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
    const bool size_matches = destination.size() == valid_bytes;
    source.close();
    destination.close();
    return copied && size_matches && remaining == 0U;
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
  static void write_u32(uint8_t* bytes, uint32_t value) noexcept {
    bytes[0U] = static_cast<uint8_t>(value);
    bytes[1U] = static_cast<uint8_t>(value >> 8U);
    bytes[2U] = static_cast<uint8_t>(value >> 16U);
    bytes[3U] = static_cast<uint8_t>(value >> 24U);
  }
  static void write_u64(uint8_t* bytes, uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
      bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
  }
  std::array<ArchiveIdentity, 128U> index_{};
  std::array<config::ConfigHash, 128U> hashes_{};
  std::array<uint8_t, 256U> copy_buffer_{};
  std::size_t index_count_{0U};
  bool recovery_ready_{false};
  ArchiveIdentity last_identity_{};
  bool have_last_identity_{false};
  ImmutableRecordBytes scan_record_{};
  ImmutableRecordBytes lookup_record_{};
  acquisition::SampleRecord probe_sample_{};
  ArchiveHealth health_{ArchiveHealth::Healthy};
#endif
  const char* gap_path_{"/sigap/gaps.bin"};
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
