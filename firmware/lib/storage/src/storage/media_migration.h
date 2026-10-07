#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "archive_codec.h"
#include "archive_store.h"

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
  std::size_t max_bytes_per_poll{32U * 1024U};
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

class IMigrationSource {
 public:
  virtual ~IMigrationSource() = default;
  virtual bool peek(ImmutableRecordBytes& record) noexcept = 0;
  virtual bool reclaim(const MigrationReceipt& receipt) noexcept = 0;
};

class IMigrationDestination {
 public:
  virtual ~IMigrationDestination() = default;
  virtual AppendStatus append(const ImmutableRecordBytes& record) noexcept = 0;
  virtual bool verify(const ArchiveIdentity& identity,
                      const std::array<uint8_t, 32U>& hash) noexcept = 0;
  virtual bool checkpoint(uint64_t& generation) noexcept = 0;
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

  bool checkpoint(uint64_t& generation) noexcept override {
    if (store_ == nullptr || !verified_record_ || !store_->checkpoint()) {
      return false;
    }
    generation = ++generation_;
    verified_record_ = false;
    return true;
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
                        MigrationBudget budget = {}) noexcept
      : source_(source), destination_(destination), source_media_(source_media),
        destination_media_(destination_media), budget_(budget) {}

  MigrationStatus migrate_one() noexcept {
    return migrate_one(budget_.max_bytes_per_poll);
  }

  // Retry after a destination checkpoint or source reclaim failure without
  // appending another copy.  This keeps the source record and its identity
  // authoritative until reclaim has succeeded.
  MigrationStatus migrate_one(std::size_t max_bytes) noexcept {
    bytes_migrated_last_poll_ = 0U;
    if (!source_.peek(scratch_)) {
      return has_pending_receipt_ ? MigrationStatus::Retryable
                                  : MigrationStatus::Idle;
    }
    if (!RecordCodec::valid(scratch_)) return MigrationStatus::Fault;

    if (scratch_.size > max_bytes) return MigrationStatus::LoggingBlocked;

    if (has_pending_receipt_) {
      if (!same_archive_identity(scratch_.identity, pending_receipt_.identity) ||
          scratch_.hash != pending_receipt_.hash) {
        // The source changed while a destination checkpoint was outstanding.
        // Never reclaim a different record with the old receipt.
        return MigrationStatus::Fault;
      }
      if (!source_.reclaim(pending_receipt_)) return MigrationStatus::Retryable;
      pending_receipt_.phase = MigrationPhase::Reclaimed;
      pending_receipt_.durable = true;
      has_pending_receipt_ = false;
      bytes_migrated_last_poll_ = scratch_.size;
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
    if (!source_.reclaim(pending_receipt_)) return MigrationStatus::Retryable;
    pending_receipt_.phase = MigrationPhase::Reclaimed;
    has_pending_receipt_ = false;
    bytes_migrated_last_poll_ = scratch_.size;
    return MigrationStatus::Durable;
  }

  std::size_t bytes_migrated_last_poll() const noexcept {
    return bytes_migrated_last_poll_;
  }

  const MigrationReceipt* pending_receipt() const noexcept {
    return has_pending_receipt_ ? &pending_receipt_ : nullptr;
  }

 private:
  IMigrationSource& source_;
  IMigrationDestination& destination_;
  ArchiveMedia source_media_{ArchiveMedia::Unknown};
  ArchiveMedia destination_media_{ArchiveMedia::Unknown};
  MigrationBudget budget_{};
  MigrationReceipt pending_receipt_{};
  std::size_t bytes_migrated_last_poll_{0U};
  bool has_pending_receipt_{false};
  // One bounded instance-owned frame avoids a 16 KiB call-stack temporary.
  ImmutableRecordBytes scratch_{};
};

}  // namespace sigap::storage
