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

struct MigrationReceipt final {
  ArchiveIdentity identity{};
  std::array<uint8_t, 32U> hash{};
  ArchiveMedia source{ArchiveMedia::Unknown};
  ArchiveMedia destination{ArchiveMedia::Unknown};
  uint64_t checkpoint_generation{0U};
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
                        ArchiveMedia destination_media = ArchiveMedia::Unknown) noexcept
      : source_(source), destination_(destination), source_media_(source_media),
        destination_media_(destination_media) {}

  MigrationStatus migrate_one() noexcept {
    if (!source_.peek(scratch_)) return MigrationStatus::Idle;
    if (!RecordCodec::valid(scratch_)) return MigrationStatus::Fault;

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
    receipt.durable = true;
    if (!source_.reclaim(receipt)) return MigrationStatus::Retryable;
    return MigrationStatus::Durable;
  }

 private:
  IMigrationSource& source_;
  IMigrationDestination& destination_;
  ArchiveMedia source_media_{ArchiveMedia::Unknown};
  ArchiveMedia destination_media_{ArchiveMedia::Unknown};
  // One bounded instance-owned frame avoids a 16 KiB call-stack temporary.
  ImmutableRecordBytes scratch_{};
};

}  // namespace sigap::storage
