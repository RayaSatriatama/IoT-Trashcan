#include <cstdint>

#include <unity.h>

#include "storage/archive_codec.h"
#include "storage/media_migration.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::storage;

DeviceId device() {
  DeviceId value{};
  value.present = true;
  value.bytes[0] = 0x31U;
  return value;
}

BootId boot() {
  BootId value{};
  value.present = true;
  value.bytes[0] = 0x42U;
  return value;
}

ImmutableRecordBytes record(uint64_t sequence) {
  SampleRecord sample{};
  sample.header.device_id = device();
  sample.header.boot_id = boot();
  sample.header.sequence = sequence;
  sample.header.sensor = SensorId::Mq4;
  sample.header.stream = StreamId::Organic;
  sample.header.kind = SampleKind::MqAdc;
  sample.header.monotonic_us = sequence * 1000U;
  sample.quality = SampleQuality::Timeout;
  sample.reason = SampleReason::Timeout;
  ImmutableRecordBytes encoded{};
  (void)RecordCodec::encode_sample(sample, encoded);
  return encoded;
}

class Source final : public IMigrationSource {
 public:
  explicit Source(const ImmutableRecordBytes& value) noexcept : value_(value) {}

  bool peek(ImmutableRecordBytes& output) noexcept override {
    if (reclaimed_) return false;
    output = value_;
    return true;
  }

  bool reclaim(const MigrationReceipt& receipt) noexcept override {
    if (reclaim_failures_ != 0U) {
      --reclaim_failures_;
      return false;
    }
    if (!receipt.durable || receipt.phase != MigrationPhase::Checkpointed ||
        !same_archive_identity(receipt.identity, value_.identity) ||
        receipt.hash != value_.hash) {
      return false;
    }
    reclaimed_ = true;
    return true;
  }

  void fail_reclaim_once() noexcept { reclaim_failures_ = 1U; }
  bool reclaimed() const noexcept { return reclaimed_; }

 private:
  ImmutableRecordBytes value_{};
  uint32_t reclaim_failures_{0U};
  bool reclaimed_{false};
};

class Destination final : public IMigrationDestination {
 public:
  AppendStatus append(const ImmutableRecordBytes& value) noexcept override {
    ++append_calls_;
    if (append_retry_) return AppendStatus::Retryable;
    value_ = value;
    return AppendStatus::Durable;
  }

  bool verify(const ArchiveIdentity& identity,
              const std::array<uint8_t, 32U>& hash) noexcept override {
    return same_archive_identity(identity, value_.identity) && hash == value_.hash;
  }

  bool checkpoint(uint64_t& generation) noexcept override {
    if (checkpoint_failures_ != 0U) {
      --checkpoint_failures_;
      return false;
    }
    generation = ++generation_;
    return true;
  }

  void fail_checkpoint_once() noexcept { checkpoint_failures_ = 1U; }
  std::size_t append_calls() const noexcept { return append_calls_; }

 private:
  ImmutableRecordBytes value_{};
  uint32_t checkpoint_failures_{0U};
  uint64_t generation_{0U};
  std::size_t append_calls_{0U};
  bool append_retry_{false};
};

class ReceiptJournal final : public IMigrationReceiptJournal {
 public:
  MigrationReceiptLoad load(MigrationReceipt& output) noexcept override {
    if (!present_) return MigrationReceiptLoad::NotFound;
    output = receipt_;
    return MigrationReceiptLoad::Found;
  }

  bool persist(const MigrationReceipt& value) noexcept override {
    receipt_ = value;
    present_ = true;
    return true;
  }

  bool clear(const MigrationReceipt& value) noexcept override {
    if (!present_ || receipt_.hash != value.hash ||
        receipt_.checkpoint_generation != value.checkpoint_generation) {
      return false;
    }
    present_ = false;
    return true;
  }

  bool present() const noexcept { return present_; }

 private:
  MigrationReceipt receipt_{};
  bool present_{false};
};

void test_budget_prevents_unbounded_migration() {
  const auto value = record(1U);
  Source source(value);
  Destination destination;
  ArchiveMediaMigration migration(source, destination);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::LoggingBlocked),
                        static_cast<int>(migration.migrate_one(value.size - 1U)));
  TEST_ASSERT_FALSE(source.reclaimed());
  TEST_ASSERT_EQUAL_UINT32(0U, destination.append_calls());
}

void test_reclaim_retry_reuses_checkpointed_receipt() {
  const auto value = record(2U);
  Source source(value);
  source.fail_reclaim_once();
  Destination destination;
  ArchiveMediaMigration migration(source, destination);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Retryable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_NOT_NULL(migration.pending_receipt());
  TEST_ASSERT_EQUAL_UINT32(1U, destination.append_calls());

  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Durable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_TRUE(source.reclaimed());
  TEST_ASSERT_EQUAL_UINT32(1U, destination.append_calls());
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(value.size),
                           static_cast<uint32_t>(migration.bytes_migrated_last_poll()));
}

void test_checkpoint_retry_preserves_source_and_receipt_is_not_faked() {
  const auto value = record(3U);
  Source source(value);
  Destination destination;
  destination.fail_checkpoint_once();
  ArchiveMediaMigration migration(source, destination);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Retryable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_NULL(migration.pending_receipt());
  TEST_ASSERT_FALSE(source.reclaimed());

  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Durable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_TRUE(source.reclaimed());
}

void test_checkpoint_receipt_reconstructs_after_runtime_restart() {
  const auto value = record(4U);
  Source source(value);
  source.fail_reclaim_once();
  Destination destination;
  ReceiptJournal journal;

  {
    ArchiveMediaMigration migration(source, destination, ArchiveMedia::LittleFs,
                                     ArchiveMedia::Sd, {}, &journal);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Retryable),
                          static_cast<int>(migration.migrate_one()));
    TEST_ASSERT_TRUE(journal.present());
    TEST_ASSERT_NOT_NULL(migration.pending_receipt());
  }

  // A recreated migration object must resume from the durable checkpoint,
  // without appending the destination record a second time.
  ArchiveMediaMigration restarted(source, destination, ArchiveMedia::LittleFs,
                                  ArchiveMedia::Sd, {}, &journal);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Durable),
                        static_cast<int>(restarted.migrate_one()));
  TEST_ASSERT_TRUE(source.reclaimed());
  TEST_ASSERT_FALSE(journal.present());
  TEST_ASSERT_EQUAL_UINT32(1U, destination.append_calls());
}

void test_migration_reports_estimated_io_budget() {
  const auto value = record(5U);
  Source source(value);
  Destination destination;
  ArchiveMediaMigration migration(source, destination);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Durable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_TRUE(migration.io_bytes_last_poll() > value.size);
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_budget_prevents_unbounded_migration);
  RUN_TEST(test_reclaim_retry_reuses_checkpointed_receipt);
  RUN_TEST(test_checkpoint_retry_preserves_source_and_receipt_is_not_faked);
  RUN_TEST(test_checkpoint_receipt_reconstructs_after_runtime_restart);
  RUN_TEST(test_migration_reports_estimated_io_budget);
  return UNITY_END();
}
