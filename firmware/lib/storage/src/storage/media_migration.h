#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "archive_codec.h"
#include "archive_store.h"

#ifdef ARDUINO
#include <FS.h>
#endif

namespace sigap::storage {

inline bool same_archive_identity(const ArchiveIdentity& left,
                                  const ArchiveIdentity& right) noexcept {
  return left.kind == right.kind && left.sequence == right.sequence &&
         left.device_id.present == right.device_id.present &&
         left.device_id.bytes == right.device_id.bytes &&
         left.boot_id.present == right.boot_id.present &&
         left.boot_id.bytes == right.boot_id.bytes;
}

enum class MigrationStatus : uint8_t {
  Idle,
  Durable,
  Retryable,
  LoggingBlocked,
  Fault,
};

enum class ArchiveMedia : uint8_t { Unknown, LittleFs, Sd };

// Migration is deliberately bounded to one record per invocation.  The
// budget is still explicit so a caller can decline a record that would make
// the current storage-worker slice exceed its byte budget.
struct MigrationBudget final {
  // Includes the source read, destination append, identity/hash verification,
  // checkpoint, and reclaim metadata writes for one bounded record.
  std::size_t max_bytes_per_poll{64U * 1024U};
};

enum class MigrationPhase : uint8_t {
  None,
  Copied,
  Verified,
  Checkpointed,
  Reclaimed,
};

struct MigrationReceipt final {
  ArchiveIdentity identity{};
  std::array<uint8_t, 32U> hash{};
  ArchiveMedia source{ArchiveMedia::Unknown};
  ArchiveMedia destination{ArchiveMedia::Unknown};
  uint64_t checkpoint_generation{0U};
  MigrationPhase phase{MigrationPhase::None};
  bool durable{false};
};

enum class MigrationReceiptLoad : uint8_t {
  NotFound,
  Found,
  Error,
};

// Optional durable metadata boundary for a checkpointed copy.  The target
// composition can inject a journal without making migration depend on a
// particular filesystem implementation.
class IMigrationReceiptJournal {
 public:
  virtual ~IMigrationReceiptJournal() = default;
  virtual MigrationReceiptLoad load(MigrationReceipt& receipt) noexcept = 0;
  virtual bool persist(const MigrationReceipt& receipt) noexcept = 0;
  virtual bool clear(const MigrationReceipt& receipt) noexcept = 0;
};

class IMigrationSource {
 public:
  virtual ~IMigrationSource() = default;
  virtual bool peek(ImmutableRecordBytes& record) noexcept = 0;
  virtual bool reclaim(const MigrationReceipt& receipt) noexcept = 0;
  virtual bool contains(const ArchiveIdentity& identity,
                        const std::array<uint8_t, 32U>& hash) noexcept {
    (void)identity;
    (void)hash;
    return false;
  }
};

class IMigrationDestination {
 public:
  virtual ~IMigrationDestination() = default;
  virtual AppendStatus append(const ImmutableRecordBytes& record) noexcept = 0;
  virtual bool verify(const ArchiveIdentity& identity,
                      const std::array<uint8_t, 32U>& hash) noexcept = 0;
  virtual bool checkpoint(uint64_t& generation) noexcept = 0;
  // A durable receipt may authorize source reclaim only while the destination
  // still recognizes the checkpoint generation that produced that copy.  The
  // default preserves existing stores whose durable checkpoint authority is
  // the receipt journal itself; backends with a generation ledger can
  // override it with a stronger check.
  virtual bool checkpoint_authorized(uint64_t generation) noexcept {
    return generation != 0U;
  }
  virtual bool contains(const ArchiveIdentity& identity,
                        const std::array<uint8_t, 32U>& hash) noexcept {
    (void)identity;
    (void)hash;
    return false;
  }
};

// Adapter used by the target composition: the source store remains the
// owner of pending bytes, while this seam exposes one record at a time.
class ArchiveStoreMigrationSource final : public IMigrationSource {
 public:
  explicit ArchiveStoreMigrationSource(
      IArchiveStore& store,
      ArchiveMedia media = ArchiveMedia::Unknown) noexcept
      : store_(store), media_(media) {}

  bool peek(ImmutableRecordBytes& record) noexcept override {
    return store_.peek_pending(record);
  }

  bool reclaim(const MigrationReceipt& receipt) noexcept override {
    return receipt.durable &&
           (receipt.source == ArchiveMedia::Unknown || receipt.source == media_) &&
           store_.reclaim_pending(receipt.identity, receipt.hash);
  }

  bool contains(const ArchiveIdentity& identity,
                const std::array<uint8_t, 32U>& hash) noexcept override {
    return store_.contains(identity, hash);
  }

 private:
  IArchiveStore& store_;
  ArchiveMedia media_{ArchiveMedia::Unknown};
};

// Destination adapter for an already composed archive store.  The store is
// still the durability authority; this adapter only retains the just-appended
// identity so verify() cannot accidentally accept an unrelated record.
class ArchiveStoreMigrationDestination final : public IMigrationDestination {
 public:
  explicit ArchiveStoreMigrationDestination(
      IArchiveStore* store = nullptr,
      ArchiveMedia media = ArchiveMedia::Unknown) noexcept
      : store_(store), media_(media) {}

  void set_store(IArchiveStore* store) noexcept { store_ = store; }
  bool configured() const noexcept { return store_ != nullptr; }

  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
    if (store_ == nullptr) return AppendStatus::Retryable;
    const auto status = store_->append(record);
    if (status == AppendStatus::Durable) {
      last_identity_ = record.identity;
      last_hash_ = record.hash;
      verified_record_ = true;
    }
    return status;
  }

  bool verify(const ArchiveIdentity& identity,
              const std::array<uint8_t, 32U>& hash) noexcept override {
    return verified_record_ && same_archive_identity(last_identity_, identity) &&
           last_hash_ == hash && store_ != nullptr && store_->contains(identity, hash);
  }

  bool contains(const ArchiveIdentity& identity,
                const std::array<uint8_t, 32U>& hash) noexcept override {
    return store_ != nullptr && store_->contains(identity, hash);
  }

  bool checkpoint(uint64_t& generation) noexcept override {
    if (store_ == nullptr || !verified_record_ || !store_->checkpoint()) {
      return false;
    }
    generation = ++generation_;
    verified_record_ = false;
    return true;
  }

  bool checkpoint_authorized(uint64_t generation) noexcept override {
    // IArchiveStore::checkpoint() is the target persistence boundary.  Its
    // in-RAM generation counter is intentionally not authoritative across a
    // reboot; a persisted receipt written after that checkpoint carries the
    // durable generation.  Recovery still independently checks identity and
    // hash in the destination store before source reclaim.
    return generation != 0U;
  }

  uint64_t generation() const noexcept { return generation_; }
  ArchiveMedia media() const noexcept { return media_; }

 private:
  IArchiveStore* store_{nullptr};
  ArchiveIdentity last_identity_{};
  std::array<uint8_t, 32U> last_hash_{};
  uint64_t generation_{0U};
  bool verified_record_{false};
  ArchiveMedia media_{ArchiveMedia::Unknown};
};

class ArchiveMediaMigration final {
 public:
  ArchiveMediaMigration(IMigrationSource& source,
                        IMigrationDestination& destination,
                        ArchiveMedia source_media = ArchiveMedia::Unknown,
                        ArchiveMedia destination_media = ArchiveMedia::Unknown,
                        MigrationBudget budget = {},
                        IMigrationReceiptJournal* journal = nullptr) noexcept
      : source_(source), destination_(destination), source_media_(source_media),
        destination_media_(destination_media), budget_(budget), journal_(journal) {}

  MigrationStatus migrate_one() noexcept {
    return migrate_one(budget_.max_bytes_per_poll);
  }

  // Retry after a destination checkpoint or source reclaim failure without
  // appending another copy.  This keeps the source record and its identity
  // authoritative until reclaim has succeeded.
  MigrationStatus migrate_one(std::size_t max_bytes) noexcept {
    bytes_migrated_last_poll_ = 0U;
    if (!initialize_journal()) return MigrationStatus::Fault;

    // Complete a source-reclaim transaction whose metadata clear was
    // interrupted.  No new source record is read until this transition ends.
    if (has_pending_receipt_ &&
        pending_receipt_.phase == MigrationPhase::Reclaimed) {
      if (journal_ != nullptr && !journal_->clear(pending_receipt_)) {
        return MigrationStatus::Retryable;
      }
      pending_receipt_ = MigrationReceipt{};
      has_pending_receipt_ = false;
      return MigrationStatus::Durable;
    }

    if (!source_.peek(scratch_)) {
      if (has_pending_receipt_ &&
          receipt_authorizes_reclaim()) {
        // Source reclaim completed immediately before the receipt clear was
        // persisted.  The destination identity is authoritative; do not
        // resurrect a forensic source copy on restart.
        if (journal_ != nullptr && !journal_->clear(pending_receipt_)) {
          return MigrationStatus::Retryable;
        }
        pending_receipt_ = MigrationReceipt{};
        has_pending_receipt_ = false;
        return MigrationStatus::Durable;
      }
      return has_pending_receipt_ ? MigrationStatus::Retryable
                                  : MigrationStatus::Idle;
    }
    if (!RecordCodec::valid(scratch_)) return MigrationStatus::Fault;

    const std::size_t estimated_io = estimated_io_bytes(scratch_.size);
    if (estimated_io > max_bytes) return MigrationStatus::LoggingBlocked;

    if (has_pending_receipt_) {
      if (!same_archive_identity(scratch_.identity, pending_receipt_.identity) ||
          scratch_.hash != pending_receipt_.hash) {
        if (destination_.contains(pending_receipt_.identity,
                                  pending_receipt_.hash) &&
            !source_.contains(pending_receipt_.identity,
                              pending_receipt_.hash)) {
          // A legitimate reclaim completed before journal cleanup.  Clear
          // only the completed receipt and keep the next source record intact.
          if (journal_ != nullptr && !journal_->clear(pending_receipt_)) {
            return MigrationStatus::Retryable;
          }
          pending_receipt_ = MigrationReceipt{};
          has_pending_receipt_ = false;
          return MigrationStatus::Durable;
        }
        // The source changed while a destination checkpoint was outstanding.
        // Never reclaim a different record with the old receipt.
        return MigrationStatus::Fault;
      }
      if (!receipt_persisted_) {
        // A checkpointed destination copy is not reclaim authority until its
        // receipt is durably persisted.  Retrying persistence is a separate
        // transaction step so a prior failed write can never be bypassed by
        // a later poll in the same runtime.
        if (!destination_has_receipt() ||
            !destination_.checkpoint_authorized(
                pending_receipt_.checkpoint_generation)) {
          return MigrationStatus::Fault;
        }
        if (journal_ != nullptr && !journal_->persist(pending_receipt_)) {
          return MigrationStatus::Retryable;
        }
        receipt_persisted_ = true;
        return MigrationStatus::Retryable;
      }
      if (!receipt_authorizes_reclaim()) return MigrationStatus::Fault;
      if (!source_.reclaim(pending_receipt_)) return MigrationStatus::Retryable;
      pending_receipt_.phase = MigrationPhase::Reclaimed;
      pending_receipt_.durable = true;
      if (journal_ != nullptr && !journal_->clear(pending_receipt_)) {
        bytes_migrated_last_poll_ = scratch_.size;
        return MigrationStatus::Retryable;
      }
      pending_receipt_ = MigrationReceipt{};
      has_pending_receipt_ = false;
      bytes_migrated_last_poll_ = scratch_.size;
      io_bytes_last_poll_ = estimated_io;
      return MigrationStatus::Durable;
    }

    const auto append_status = destination_.append(scratch_);
    if (append_status == AppendStatus::Retryable) return MigrationStatus::Retryable;
    if (append_status == AppendStatus::LoggingBlocked) {
      return MigrationStatus::LoggingBlocked;
    }
    if (append_status != AppendStatus::Durable) return MigrationStatus::Fault;
    if (!destination_.verify(scratch_.identity, scratch_.hash)) {
      return MigrationStatus::Retryable;
    }

    uint64_t generation{0U};
    if (!destination_.checkpoint(generation)) return MigrationStatus::Retryable;
    MigrationReceipt receipt{};
    receipt.identity = scratch_.identity;
    receipt.hash = scratch_.hash;
    receipt.source = source_media_;
    receipt.destination = destination_media_;
    receipt.checkpoint_generation = generation;
    receipt.phase = MigrationPhase::Checkpointed;
    receipt.durable = true;
    pending_receipt_ = receipt;
    has_pending_receipt_ = true;
    receipt_persisted_ = journal_ == nullptr;
    if (journal_ != nullptr && !journal_->persist(pending_receipt_)) {
      return MigrationStatus::Retryable;
    }
    receipt_persisted_ = true;
    if (!source_.reclaim(pending_receipt_)) return MigrationStatus::Retryable;
    pending_receipt_.phase = MigrationPhase::Reclaimed;
    if (journal_ != nullptr && !journal_->clear(pending_receipt_)) {
      bytes_migrated_last_poll_ = scratch_.size;
      io_bytes_last_poll_ = estimated_io;
      return MigrationStatus::Retryable;
    }
    pending_receipt_ = MigrationReceipt{};
    has_pending_receipt_ = false;
    bytes_migrated_last_poll_ = scratch_.size;
    io_bytes_last_poll_ = estimated_io;
    return MigrationStatus::Durable;
  }

  std::size_t bytes_migrated_last_poll() const noexcept {
    return bytes_migrated_last_poll_;
  }

  std::size_t io_bytes_last_poll() const noexcept { return io_bytes_last_poll_; }

  const MigrationReceipt* pending_receipt() const noexcept {
    return has_pending_receipt_ ? &pending_receipt_ : nullptr;
  }

 private:
  bool initialize_journal() noexcept {
    if (journal_ == nullptr || journal_loaded_) return true;
    MigrationReceipt persisted{};
    const auto status = journal_->load(persisted);
    if (status == MigrationReceiptLoad::Error) return false;
    if (status == MigrationReceiptLoad::Found) {
      if (!persisted.durable ||
          persisted.phase != MigrationPhase::Checkpointed ||
          persisted.checkpoint_generation == 0U) {
        return false;
      }
      pending_receipt_ = persisted;
      has_pending_receipt_ = true;
      receipt_persisted_ = true;
    }
    journal_loaded_ = true;
    return true;
  }

  bool receipt_authorizes_reclaim() noexcept {
    return has_pending_receipt_ && receipt_persisted_ &&
           pending_receipt_.durable &&
           pending_receipt_.phase == MigrationPhase::Checkpointed &&
           pending_receipt_.checkpoint_generation != 0U &&
           destination_has_receipt() &&
           destination_.checkpoint_authorized(
               pending_receipt_.checkpoint_generation);
  }

  bool destination_has_receipt() noexcept {
    return destination_.contains(pending_receipt_.identity,
                                 pending_receipt_.hash) ||
           destination_.verify(pending_receipt_.identity,
                               pending_receipt_.hash);
  }

  static constexpr std::size_t estimated_io_bytes(std::size_t record_bytes) noexcept {
    constexpr std::size_t kIdentityBytes = sizeof(ArchiveIdentity);
    constexpr std::size_t kHashBytes = 32U;
    constexpr std::size_t kMetadataBytes = 32U;
    return record_bytes + record_bytes + kIdentityBytes + kHashBytes + kMetadataBytes;
  }

  IMigrationSource& source_;
  IMigrationDestination& destination_;
  ArchiveMedia source_media_{ArchiveMedia::Unknown};
  ArchiveMedia destination_media_{ArchiveMedia::Unknown};
  MigrationBudget budget_{};
  MigrationReceipt pending_receipt_{};
  std::size_t bytes_migrated_last_poll_{0U};
  std::size_t io_bytes_last_poll_{0U};
  bool has_pending_receipt_{false};
  bool receipt_persisted_{false};
  IMigrationReceiptJournal* journal_{nullptr};
  bool journal_loaded_{false};
  // One bounded instance-owned frame avoids a 16 KiB call-stack temporary.
  ImmutableRecordBytes scratch_{};
};

#ifdef ARDUINO
// Durable migration authority.  The fixed frame binds source/destination,
// checkpoint generation, identity, and content hash.  A forensic quarantine
// is never treated as pending again unless this receipt still authorizes it.
class FsMigrationReceiptJournal final : public IMigrationReceiptJournal {
 public:
  static constexpr std::size_t kFrameBytes{112U};
  static constexpr uint32_t kMagic{0x5349474DU};
  static constexpr uint32_t kMarker{0x4D494752U};
  static constexpr uint8_t kVersion{1U};

  explicit FsMigrationReceiptJournal(
      const char* path = "/sigap/archive.migration") noexcept
      : path_(path) {}

  bool begin(fs::FS& filesystem) noexcept {
    filesystem_ = &filesystem;
    return true;
  }

  MigrationReceiptLoad load(MigrationReceipt& output) noexcept override {
    if (filesystem_ == nullptr) return MigrationReceiptLoad::Error;
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) return MigrationReceiptLoad::NotFound;
    if (file.size() != kFrameBytes) {
      file.close();
      return MigrationReceiptLoad::Error;
    }
    std::array<uint8_t, kFrameBytes> frame{};
    const bool read_ok = file.read(frame.data(), frame.size()) ==
                         static_cast<int>(frame.size());
    file.close();
    if (!read_ok || !decode(frame, output)) return MigrationReceiptLoad::Error;
    return MigrationReceiptLoad::Found;
  }

  bool persist(const MigrationReceipt& receipt) noexcept override {
    if (filesystem_ == nullptr || !valid_receipt(receipt)) return false;
    const auto frame = encode(receipt);
    File file = filesystem_->open(path_, FILE_WRITE);
    if (!file) return false;
    const bool written = file.write(frame.data(), frame.size()) ==
                         static_cast<int>(frame.size());
    file.close();
    return written;
  }

  bool clear(const MigrationReceipt& receipt) noexcept override {
    MigrationReceipt current{};
    const auto loaded = load(current);
    if (loaded == MigrationReceiptLoad::NotFound) return true;
    if (loaded != MigrationReceiptLoad::Found ||
        !same_archive_identity(current.identity, receipt.identity) ||
        current.hash != receipt.hash ||
        current.checkpoint_generation != receipt.checkpoint_generation) {
      return false;
    }
    return filesystem_ != nullptr && filesystem_->remove(path_);
  }

 private:
  static void put_u32(uint8_t* target, uint32_t value) noexcept {
    target[0U] = static_cast<uint8_t>(value);
    target[1U] = static_cast<uint8_t>(value >> 8U);
    target[2U] = static_cast<uint8_t>(value >> 16U);
    target[3U] = static_cast<uint8_t>(value >> 24U);
  }
  static void put_u64(uint8_t* target, uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
      target[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
  }
  static uint32_t get_u32(const uint8_t* source) noexcept {
    return static_cast<uint32_t>(source[0U]) |
           (static_cast<uint32_t>(source[1U]) << 8U) |
           (static_cast<uint32_t>(source[2U]) << 16U) |
           (static_cast<uint32_t>(source[3U]) << 24U);
  }
  static uint64_t get_u64(const uint8_t* source) noexcept {
    uint64_t value{0U};
    for (std::size_t index = 0U; index < 8U; ++index) {
      value |= static_cast<uint64_t>(source[index]) << (index * 8U);
    }
    return value;
  }
  static bool valid_receipt(const MigrationReceipt& receipt) noexcept {
    return receipt.durable && receipt.phase == MigrationPhase::Checkpointed &&
           receipt.checkpoint_generation != 0U &&
           receipt.source != ArchiveMedia::Unknown &&
           receipt.destination != ArchiveMedia::Unknown &&
           receipt.identity.device_id.present && receipt.identity.boot_id.present;
  }
  static std::array<uint8_t, kFrameBytes> encode(
      const MigrationReceipt& receipt) noexcept {
    std::array<uint8_t, kFrameBytes> frame{};
    put_u32(frame.data(), kMagic);
    frame[4U] = kVersion;
    frame[5U] = static_cast<uint8_t>(receipt.phase);
    frame[6U] = receipt.durable ? 1U : 0U;
    frame[7U] = static_cast<uint8_t>(receipt.source);
    frame[8U] = static_cast<uint8_t>(receipt.destination);
    put_u64(frame.data() + 9U, receipt.checkpoint_generation);
    frame[17U] = static_cast<uint8_t>(receipt.identity.kind);
    put_u64(frame.data() + 18U, receipt.identity.sequence);
    frame[26U] = receipt.identity.device_id.present ? 1U : 0U;
    std::memcpy(frame.data() + 27U, receipt.identity.device_id.bytes.data(), 16U);
    frame[43U] = receipt.identity.boot_id.present ? 1U : 0U;
    std::memcpy(frame.data() + 44U, receipt.identity.boot_id.bytes.data(), 16U);
    std::memcpy(frame.data() + 60U, receipt.hash.data(), receipt.hash.size());
    put_u32(frame.data() + 92U, archive_crc32(frame.data(), 92U));
    put_u32(frame.data() + 96U, kMarker);
    return frame;
  }
  static bool decode(const std::array<uint8_t, kFrameBytes>& frame,
                     MigrationReceipt& output) noexcept {
    if (get_u32(frame.data()) != kMagic || frame[4U] != kVersion ||
        get_u32(frame.data() + 92U) != archive_crc32(frame.data(), 92U) ||
        get_u32(frame.data() + 96U) != kMarker ||
        frame[7U] > static_cast<uint8_t>(ArchiveMedia::Sd) ||
        frame[8U] > static_cast<uint8_t>(ArchiveMedia::Sd) ||
        frame[5U] != static_cast<uint8_t>(MigrationPhase::Checkpointed) ||
        frame[6U] == 0U || get_u64(frame.data() + 9U) == 0U) return false;
    output = MigrationReceipt{};
    output.phase = static_cast<MigrationPhase>(frame[5U]);
    output.durable = true;
    output.source = static_cast<ArchiveMedia>(frame[7U]);
    output.destination = static_cast<ArchiveMedia>(frame[8U]);
    output.checkpoint_generation = get_u64(frame.data() + 9U);
    output.identity.kind = static_cast<ArchiveRecordKind>(frame[17U]);
    output.identity.sequence = get_u64(frame.data() + 18U);
    output.identity.device_id.present = frame[26U] != 0U;
    output.identity.boot_id.present = frame[43U] != 0U;
    std::memcpy(output.identity.device_id.bytes.data(), frame.data() + 27U, 16U);
    std::memcpy(output.identity.boot_id.bytes.data(), frame.data() + 44U, 16U);
    std::memcpy(output.hash.data(), frame.data() + 60U, output.hash.size());
    return output.identity.device_id.present && output.identity.boot_id.present;
  }

  fs::FS* filesystem_{nullptr};
  const char* path_{nullptr};
};
#endif

}  // namespace sigap::storage
