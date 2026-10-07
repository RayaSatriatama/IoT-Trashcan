#include <cstdint>
#include <cstdio>

#include <unity.h>

#include "acquisition/acquisition_types.h"
#include "storage/archive_codec.h"
#include "storage/archive_media.h"
#include "storage/archive_store.h"
#include "storage/media_migration.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::storage;

class MigrationSource final : public IMigrationSource {
 public:
  explicit MigrationSource(const ImmutableRecordBytes& value) noexcept
      : record_(value) {}
  bool peek(ImmutableRecordBytes& output) noexcept override {
    if (reclaimed_) return false;
    output = record_;
    return true;
  }
  bool reclaim(const MigrationReceipt& receipt) noexcept override {
    if (!receipt.durable || receipt.identity.sequence != record_.identity.sequence ||
        receipt.hash != record_.hash) return false;
    reclaimed_ = true;
    return true;
  }
  bool reclaimed() const noexcept { return reclaimed_; }

 private:
  ImmutableRecordBytes record_{};
  bool reclaimed_{false};
};

class MigrationDestination final : public IMigrationDestination {
 public:
  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
    if (fail_append_) return AppendStatus::Retryable;
    record_ = record;
    return AppendStatus::Durable;
  }
  bool verify(const ArchiveIdentity& identity,
              const std::array<uint8_t, 32U>& hash) noexcept override {
    return record_.identity.sequence == identity.sequence && record_.hash == hash;
  }
  bool checkpoint(uint64_t& generation) noexcept override {
    if (fail_checkpoint_) return false;
    generation = ++generation_;
    return true;
  }
  void set_fail_append(bool value) noexcept { fail_append_ = value; }
  void set_fail_checkpoint(bool value) noexcept { fail_checkpoint_ = value; }

 private:
  ImmutableRecordBytes record_{};
  uint64_t generation_{0U};
  bool fail_append_{false};
  bool fail_checkpoint_{false};
};

DeviceId device() {
  DeviceId value{};
  value.present = true;
  value.bytes[0] = 0x11U;
  return value;
}

BootId boot() {
  BootId value{};
  value.present = true;
  value.bytes[0] = 0x22U;
  return value;
}

ConfigRef config_ref() {
  ConfigRef value{};
  value.present = true;
  value.schema_version = 1U;
  value.config_version = 3U;
  value.hash[0] = 0x44U;
  return value;
}

SampleRecord sample(uint64_t sequence) {
  SampleRecord value{};
  value.header.device_id = device();
  value.header.boot_id = boot();
  value.header.sequence = sequence;
  value.header.sensor = SensorId::Mq4;
  value.header.stream = StreamId::Organic;
  value.header.kind = SampleKind::MqAdc;
  value.header.monotonic_us = sequence * 1000U;
  value.header.config_ref.present = true;
  value.header.config_ref.schema_version = 1U;
  value.header.config_ref.config_version = 3U;
  value.quality = SampleQuality::Ready;
  value.reason = SampleReason::DataReady;
  value.raw.present = true;
  value.raw.adc_counts = 1234U;
  value.raw.temperature_crc_valid = true;
  value.raw.humidity_crc_valid = false;
  value.raw.heater_ready = true;
  return value;
}

ImmutableRecordBytes encoded(uint64_t sequence) {
  ImmutableRecordBytes value{};
  (void)RecordCodec::encode_sample(sample(sequence), value);
  return value;
}

ImmutableRecordBytes encoded_kind(uint64_t sequence, ArchiveRecordKind kind) {
  ImmutableRecordBytes value{};
  const ArchiveIdentity identity{device(), boot(), sequence, kind};
  PayloadWriter writer{nullptr, 0U};
  if (!RecordCodec::begin(identity, value, writer)) return value;
  writer = PayloadWriter(value.bytes.data() + kArchiveHeaderBytes,
                         value.bytes.size() - kArchiveHeaderBytes -
                             kArchiveTrailerBytes);
  const auto cfg = config_ref();
  if (!writer.u8(static_cast<uint8_t>(kind)) ||
      !writer.u64(sequence) ||
      !writer.u64(sequence * 1000U) ||
      !writer.bytes(device().bytes.data(), device().bytes.size()) ||
      !writer.bytes(boot().bytes.data(), boot().bytes.size()) ||
      !writer.u32(cfg.schema_version) ||
      !writer.u64(cfg.config_version) ||
      !writer.bytes(cfg.hash.data(), cfg.hash.size()) ||
      !RecordCodec::finish(value, writer)) return ImmutableRecordBytes{};
  return value;
}

void test_framing_rejects_corrupt_and_partial_tail() {
  const auto full = encoded(1U);
  TEST_ASSERT_TRUE(RecordCodec::valid(full));
  auto corrupt = full;
  corrupt.bytes[corrupt.size - 1U] ^= 0x01U;
  TEST_ASSERT_FALSE(RecordCodec::valid(corrupt));
  TEST_ASSERT_FALSE(RecordCodec::valid_frame(full.bytes.data(), full.size - 1U));
}

void test_codec_reader_round_trips_identity_and_payload() {
  const auto record = encoded(12U);
  ArchiveRecordView view{};
  TEST_ASSERT_TRUE(RecordCodec::read(record, view));
  TEST_ASSERT_EQUAL_UINT64(12U, view.identity.sequence);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArchiveRecordKind::Sample),
                          static_cast<uint8_t>(view.identity.kind));
  TEST_ASSERT_TRUE(view.identity.device_id.present);
  TEST_ASSERT_TRUE(view.identity.boot_id.present);
  TEST_ASSERT_EQUAL_UINT32(
      static_cast<uint32_t>(record.size - kArchiveHeaderBytes - kArchiveTrailerBytes),
      static_cast<uint32_t>(view.payload_size));
  TEST_ASSERT_EQUAL_UINT8(record.bytes[kArchiveHeaderBytes], view.payload[0]);
  SampleRecord decoded{};
  TEST_ASSERT_TRUE(RecordCodec::decode_sample(record, decoded));
  TEST_ASSERT_EQUAL_UINT64(12000U, decoded.header.monotonic_us);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SampleQuality::Ready),
                          static_cast<uint8_t>(decoded.quality));
  TEST_ASSERT_EQUAL_UINT16(1234U, decoded.raw.adc_counts);
  TEST_ASSERT_TRUE(decoded.raw.temperature_crc_valid);
  TEST_ASSERT_FALSE(decoded.raw.humidity_crc_valid);
  TEST_ASSERT_TRUE(decoded.raw.heater_ready);
}

void test_typed_views_reject_wrong_archive_kinds() {
  const auto result = encoded_kind(40U, ArchiveRecordKind::Result);
  const auto event = encoded_kind(41U, ArchiveRecordKind::Event);
  const auto metadata = encoded_kind(42U, ArchiveRecordKind::Metadata);
  TypedArchiveRecordView view{};
  TEST_ASSERT_TRUE(RecordCodec::decode_result_view(result, view));
  TEST_ASSERT_FALSE(RecordCodec::decode_event_view(result, view));
  TEST_ASSERT_TRUE(RecordCodec::decode_event_view(event, view));
  TEST_ASSERT_TRUE(RecordCodec::decode_metadata_view(metadata, view));
  TEST_ASSERT_FALSE(RecordCodec::decode_result_view(metadata, view));
}

void test_store_deduplicates_and_recovers_corrupt_tail() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  const auto first = encoded(2U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
  const auto second = encoded(3U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(second)));
  store.corrupt_last_byte();
  TEST_ASSERT_TRUE(store.recover());
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
  TEST_ASSERT_TRUE(RecordCodec::valid(store.at(0U)));
}

void test_store_faults_are_observable_and_quota_does_not_overwrite() {
  const auto first = encoded(4U);
  MemoryArchiveStore<4U> quota_store(first.size);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(quota_store.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::LoggingBlocked),
                        static_cast<int>(quota_store.append(encoded(5U))));
  TEST_ASSERT_EQUAL_UINT32(1U, quota_store.size());

  MemoryArchiveStore<4U> fault_store(64U * 1024U);
  fault_store.set_fault(StoreFault::ShortWrite);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Retryable),
                        static_cast<int>(fault_store.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ArchiveHealth::Degraded),
                        static_cast<int>(fault_store.health()));
}

void test_same_identity_with_changed_content_is_conflict() {
  MemoryArchiveStore<4U> store(64U * 1024U);
  const auto first = encoded(6U);
  auto changed_sample = sample(6U);
  changed_sample.raw.adc_counts = 4321U;
  ImmutableRecordBytes changed{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(changed_sample, changed));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Invalid),
                        static_cast<int>(store.append(changed)));
}

void test_gap_tracker_coalesces_and_counts_overflow() {
  GapTracker<1U> gaps{};
  TEST_ASSERT_TRUE(gaps.record(10U, GapReason::MediaFailure));
  TEST_ASSERT_TRUE(gaps.record(11U, GapReason::MediaFailure));
  TEST_ASSERT_FALSE(gaps.record(20U, GapReason::QuotaFull));
  TEST_ASSERT_EQUAL_UINT64(3U, gaps.unlogged_records());
  TEST_ASSERT_EQUAL_UINT64(1U, gaps.overflow_count());
  TEST_ASSERT_EQUAL_UINT64(10U, gaps.at(0U).first_sequence);
  TEST_ASSERT_EQUAL_UINT64(11U, gaps.at(0U).last_sequence);
}

void test_serialized_mixed_workload_budget_is_bounded() {
  std::printf("memory ArchiveIdentity=%zu ImmutableRecordBytes=%zu MemoryArchiveStore=%zu "
              "GapTracker32=%zu LittleFsArchiveStore=%zu MediaFailoverStore=%zu\n",
              sizeof(ArchiveIdentity), sizeof(ImmutableRecordBytes),
              sizeof(MemoryArchiveStore<128U>), sizeof(GapTracker<32U>),
              sizeof(LittleFsArchiveStore), sizeof(MediaFailoverStore));
  MemoryArchiveStore<128U> store(2U * 1024U * 1024U);
  for (uint64_t sequence = 0U; sequence < 75U; ++sequence) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                          static_cast<int>(store.append(encoded(sequence))));
  }
  for (uint64_t sequence = 75U; sequence < 78U; ++sequence) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                          static_cast<int>(store.append(encoded_kind(
                              sequence, ArchiveRecordKind::Result))));
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(encoded_kind(
                            78U, ArchiveRecordKind::Event))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(encoded_kind(
                            79U, ArchiveRecordKind::Metadata))));
  TEST_ASSERT_TRUE(store.pending_bytes() < store.pending_quota_bytes());
  TEST_ASSERT_EQUAL_UINT32(80U, store.size());
}

void test_media_failover_keeps_primary_pending_and_uses_littlefs_fallback() {
  MemoryArchiveStore<4U> sd(64U * 1024U);
  MemoryArchiveStore<4U> littlefs(64U * 1024U);
  MediaFailoverStore router(sd, littlefs);
  sd.set_fault(StoreFault::MediaFailure);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(router.append(encoded(80U))));
  TEST_ASSERT_EQUAL_UINT32(0U, sd.size());
  TEST_ASSERT_EQUAL_UINT32(1U, littlefs.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ArchiveHealth::Degraded),
                        static_cast<int>(router.health()));
}

void test_media_failover_persists_gap_to_reserve_fallback() {
  MemoryArchiveStore<4U> sd(64U * 1024U);
  MemoryArchiveStore<4U> littlefs(64U * 1024U);
  MediaFailoverStore router(sd, littlefs);
  sd.set_fault(StoreFault::MediaFailure);
  const auto evidence = sample(81U);
  GapRange gap{};
  gap.first_sequence = evidence.header.sequence;
  gap.last_sequence = evidence.header.sequence;
  gap.device_id = evidence.header.device_id;
  gap.boot_id = evidence.header.boot_id;
  gap.sensor = evidence.header.sensor;
  gap.stream = evidence.header.stream;
  gap.first_monotonic_us = evidence.header.monotonic_us;
  gap.last_monotonic_us = evidence.header.monotonic_us;
  gap.record_count = 1U;
  gap.reason = GapReason::MediaFailure;
  gap.present = true;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(router.append_gap(gap)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ArchiveHealth::Degraded),
                        static_cast<int>(router.health()));
}

void test_filesystem_double_recovers_tail_and_rebuilds_index() {
  ArchiveFileSystemDouble<8U, 64U * 1024U> filesystem{};
  NativeArchiveFileStore<8U, 64U * 1024U> first_store(filesystem);
  TEST_ASSERT_TRUE(first_store.recover());
  const auto first = encoded(90U);
  const auto second = encoded(91U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(first_store.append(first)));
  filesystem.append_partial_byte(0xA5U);
  TEST_ASSERT_TRUE(first_store.recover());
  TEST_ASSERT_EQUAL_UINT32(1U, first_store.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(first_store.append(second)));

  NativeArchiveFileStore<8U, 64U * 1024U> restarted(filesystem);
  TEST_ASSERT_TRUE(restarted.recover());
  TEST_ASSERT_EQUAL_UINT32(2U, restarted.size());
  TEST_ASSERT_EQUAL_UINT64(first.size + second.size, restarted.pending_bytes());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(restarted.append(second)));
  filesystem.set_flush_failure(true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Retryable),
                        static_cast<int>(restarted.append(encoded(92U))));
}

void test_filesystem_double_distinguishes_missing_from_open_error() {
  ArchiveFileSystemDouble<4U, 16U * 1024U> missing_fs{};
  NativeArchiveFileStore<4U, 16U * 1024U> missing_store(missing_fs);
  TEST_ASSERT_TRUE(missing_store.recover());
  missing_fs.set_open_failure(true);
  TEST_ASSERT_FALSE(missing_store.recover());
}

void test_filesystem_archive_pages_beyond_index_cache_and_restarts() {
  // The target adapter keeps only a bounded identity cache.  Durable archive
  // capacity is governed by quota/media bytes, not by that cache size.
  ArchiveFileSystemDouble<128U, 2U * 1024U * 1024U> filesystem{};
  NativeArchiveFileStore<128U, 2U * 1024U * 1024U> first_store(filesystem);
  TEST_ASSERT_TRUE(first_store.recover());
  for (uint64_t sequence = 0U; sequence < 129U; ++sequence) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AppendStatus::Durable),
        static_cast<int>(first_store.append(encoded(sequence + 200U))));
  }
  TEST_ASSERT_EQUAL_UINT32(129U, first_store.size());

  NativeArchiveFileStore<128U, 2U * 1024U * 1024U> restarted(filesystem);
  TEST_ASSERT_TRUE(restarted.recover());
  TEST_ASSERT_EQUAL_UINT32(129U, restarted.size());
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(AppendStatus::Durable),
      static_cast<int>(restarted.append(encoded(328U))));
  TEST_ASSERT_EQUAL_UINT32(129U, restarted.size());

  const auto next = encoded(329U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(restarted.append(next)));
  TEST_ASSERT_EQUAL_UINT32(130U, restarted.size());

  NativeArchiveFileStore<128U, 2U * 1024U * 1024U> reconstructed(filesystem);
  TEST_ASSERT_TRUE(reconstructed.recover());
  TEST_ASSERT_EQUAL_UINT32(130U, reconstructed.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(reconstructed.append(next)));
  TEST_ASSERT_EQUAL_UINT32(130U, reconstructed.size());
}

void test_filesystem_duplicate_frame_is_deduplicated_across_restart() {
  ArchiveFileSystemDouble<8U, 64U * 1024U> filesystem{};
  NativeArchiveFileStore<8U, 64U * 1024U> store(filesystem);
  const auto first = encoded(131U);
  const auto second = encoded(132U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  TEST_ASSERT_TRUE(filesystem.append(first.bytes.data(), first.size));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(second)));
  const uint64_t logical_bytes = store.pending_bytes();
  TEST_ASSERT_TRUE(store.recover());
  TEST_ASSERT_EQUAL_UINT32(2U, store.size());
  TEST_ASSERT_EQUAL_UINT64(logical_bytes, store.pending_bytes());
  NativeArchiveFileStore<8U, 64U * 1024U> restarted(filesystem);
  TEST_ASSERT_TRUE(restarted.recover());
  TEST_ASSERT_EQUAL_UINT32(2U, restarted.size());
  TEST_ASSERT_EQUAL_UINT64(logical_bytes, restarted.pending_bytes());
}

void test_filesystem_conflict_keeps_valid_records_and_marks_degraded() {
  ArchiveFileSystemDouble<8U, 64U * 1024U> filesystem{};
  NativeArchiveFileStore<8U, 64U * 1024U> store(filesystem);
  const auto first = encoded(133U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  auto conflict_sample = sample(133U);
  conflict_sample.raw.adc_counts = 4321U;
  ImmutableRecordBytes conflict{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(conflict_sample, conflict));
  TEST_ASSERT_TRUE(filesystem.append(conflict.bytes.data(), conflict.size));
  const auto following = encoded(134U);
  TEST_ASSERT_TRUE(filesystem.append(following.bytes.data(), following.size));

  NativeArchiveFileStore<8U, 64U * 1024U> restarted(filesystem);
  TEST_ASSERT_TRUE(restarted.recover());
  TEST_ASSERT_EQUAL_UINT32(2U, restarted.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ArchiveHealth::Degraded),
                        static_cast<int>(restarted.health()));
  TEST_ASSERT_TRUE(restarted.contains(first.identity, first.hash));
  TEST_ASSERT_TRUE(restarted.contains(following.identity, following.hash));
}

void test_serialized_sixty_second_workload_reconciles_all_identities() {
  constexpr std::size_t kRecords = 4680U;
  constexpr std::size_t kBytes = 6U * 1024U * 1024U;
  static ArchiveFileSystemDouble<5000U, kBytes> filesystem{};
  static NativeArchiveFileStore<5000U, kBytes> store(filesystem);
  TEST_ASSERT_TRUE(store.recover());
  for (uint64_t sequence = 0U; sequence < kRecords; ++sequence) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                          static_cast<int>(store.append(encoded(sequence))));
  }
  TEST_ASSERT_EQUAL_UINT32(kRecords, store.size());
  TEST_ASSERT_TRUE(store.pending_bytes() < store.pending_quota_bytes());

  NativeArchiveFileStore<5000U, kBytes> restarted(filesystem);
  TEST_ASSERT_TRUE(restarted.recover());
  TEST_ASSERT_EQUAL_UINT32(kRecords, restarted.size());
  TEST_ASSERT_EQUAL_UINT64(store.pending_bytes(), restarted.pending_bytes());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(restarted.append(encoded(4679U))));
  TEST_ASSERT_EQUAL_UINT32(kRecords, restarted.size());
}

void test_media_migration_requires_verify_checkpoint_before_reclaim() {
  const auto record = encoded(100U);
  MigrationSource source(record);
  MigrationDestination destination;
  ArchiveMediaMigration migration(source, destination);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Durable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_TRUE(source.reclaimed());
}

void test_media_migration_keeps_source_on_checkpoint_failure() {
  const auto record = encoded(101U);
  MigrationSource source(record);
  MigrationDestination destination;
  destination.set_fail_checkpoint(true);
  ArchiveMediaMigration migration(source, destination);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Retryable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_FALSE(source.reclaimed());
}

void test_store_migration_adapters_copy_verify_checkpoint_and_reclaim() {
  ArchiveFileSystemDouble<8U, 64U * 1024U> source_fs{};
  ArchiveFileSystemDouble<8U, 64U * 1024U> destination_fs{};
  NativeArchiveFileStore<8U, 64U * 1024U> source_store(source_fs);
  NativeArchiveFileStore<8U, 64U * 1024U> destination_store(destination_fs);
  const auto record = encoded(140U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(source_store.append(record)));
  ArchiveStoreMigrationSource source(source_store, ArchiveMedia::LittleFs);
  ArchiveStoreMigrationDestination destination(&destination_store,
                                               ArchiveMedia::Sd);
  ArchiveMediaMigration migration(source, destination, ArchiveMedia::LittleFs,
                                  ArchiveMedia::Sd);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationStatus::Durable),
                        static_cast<int>(migration.migrate_one()));
  TEST_ASSERT_EQUAL_UINT32(0U, source_store.size());
  TEST_ASSERT_EQUAL_UINT32(1U, destination_store.size());
  NativeArchiveFileStore<8U, 64U * 1024U> restarted(destination_fs);
  TEST_ASSERT_TRUE(restarted.recover());
  TEST_ASSERT_EQUAL_UINT32(1U, restarted.size());
  TEST_ASSERT_TRUE(restarted.contains(record.identity, record.hash));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_framing_rejects_corrupt_and_partial_tail);
  RUN_TEST(test_codec_reader_round_trips_identity_and_payload);
  RUN_TEST(test_typed_views_reject_wrong_archive_kinds);
  RUN_TEST(test_store_deduplicates_and_recovers_corrupt_tail);
  RUN_TEST(test_store_faults_are_observable_and_quota_does_not_overwrite);
  RUN_TEST(test_same_identity_with_changed_content_is_conflict);
  RUN_TEST(test_gap_tracker_coalesces_and_counts_overflow);
  RUN_TEST(test_serialized_mixed_workload_budget_is_bounded);
  RUN_TEST(test_media_failover_keeps_primary_pending_and_uses_littlefs_fallback);
  RUN_TEST(test_media_failover_persists_gap_to_reserve_fallback);
  RUN_TEST(test_filesystem_double_recovers_tail_and_rebuilds_index);
  RUN_TEST(test_filesystem_double_distinguishes_missing_from_open_error);
  RUN_TEST(test_filesystem_archive_pages_beyond_index_cache_and_restarts);
  RUN_TEST(test_filesystem_duplicate_frame_is_deduplicated_across_restart);
  RUN_TEST(test_filesystem_conflict_keeps_valid_records_and_marks_degraded);
  RUN_TEST(test_serialized_sixty_second_workload_reconciles_all_identities);
  RUN_TEST(test_media_migration_requires_verify_checkpoint_before_reclaim);
  RUN_TEST(test_media_migration_keeps_source_on_checkpoint_failure);
  RUN_TEST(test_store_migration_adapters_copy_verify_checkpoint_and_reclaim);
  return UNITY_END();
}
