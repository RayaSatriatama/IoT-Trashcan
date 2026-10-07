#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "archive_codec.h"
#include "archive_store.h"

namespace sigap::storage {

enum class MigrationStatus : uint8_t {
  Idle,
  Durable,
  Retryable,
  LoggingBlocked,
  Fault,
};

struct MigrationReceipt final {
  ArchiveIdentity identity{};
  std::array<uint8_t, 32U> hash{};
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

class ArchiveMediaMigration final {
 public:
  ArchiveMediaMigration(IMigrationSource& source,
                        IMigrationDestination& destination) noexcept
      : source_(source), destination_(destination) {}

  MigrationStatus migrate_one() noexcept {
    ImmutableRecordBytes record{};
    if (!source_.peek(record)) return MigrationStatus::Idle;
    if (!RecordCodec::valid(record)) return MigrationStatus::Fault;

    const auto append_status = destination_.append(record);
    if (append_status == AppendStatus::Retryable) return MigrationStatus::Retryable;
    if (append_status == AppendStatus::LoggingBlocked) {
      return MigrationStatus::LoggingBlocked;
    }
    if (append_status != AppendStatus::Durable) return MigrationStatus::Fault;
    if (!destination_.verify(record.identity, record.hash)) {
      return MigrationStatus::Retryable;
    }

    uint64_t generation{0U};
    if (!destination_.checkpoint(generation)) return MigrationStatus::Retryable;
    MigrationReceipt receipt{};
    receipt.identity = record.identity;
    receipt.hash = record.hash;
    receipt.checkpoint_generation = generation;
    receipt.durable = true;
    if (!source_.reclaim(receipt)) return MigrationStatus::Retryable;
    return MigrationStatus::Durable;
  }

 private:
  IMigrationSource& source_;
  IMigrationDestination& destination_;
};

}  // namespace sigap::storage
