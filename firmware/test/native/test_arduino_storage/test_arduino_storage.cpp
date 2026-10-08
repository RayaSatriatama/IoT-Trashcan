#define ARDUINO 1

#include <array>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include "storage/archive_media.h"
#include "telemetry_archive/ack_journal_store.h"

namespace {

using sigap::acquisition::BootId;
using sigap::acquisition::DeviceId;
using sigap::acquisition::SampleKind;
using sigap::acquisition::SampleQuality;
using sigap::acquisition::SampleReason;
using sigap::acquisition::SampleRecord;
using sigap::acquisition::SensorId;
using sigap::acquisition::StreamId;
using sigap::storage::AppendStatus;
using sigap::storage::ImmutableRecordBytes;
using sigap::storage::LittleFsArchiveStore;
using sigap::telemetry_archive::Ack;
using sigap::telemetry_archive::AckStatus;
using sigap::telemetry_archive::AckPersistStatus;
using sigap::telemetry_archive::FsAckJournalStore;
using sigap::telemetry_archive::ImmutableBatch;

DeviceId device() {
  DeviceId value{};
  value.present = true;
  value.bytes[0U] = 0x61U;
  return value;
}

BootId boot() {
  BootId value{};
  value.present = true;
  value.bytes[0U] = 0x71U;
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
  sample.header.config_ref.present = true;
  sample.header.config_ref.schema_version = 1U;
  sample.header.config_ref.config_version = 4U;
  sample.quality = SampleQuality::Ready;
  sample.reason = SampleReason::DataReady;
  sample.raw.present = true;
  sample.raw.adc_counts = static_cast<uint32_t>(100U + sequence);
  ImmutableRecordBytes encoded{};
  (void)sigap::storage::RecordCodec::encode_sample(sample, encoded);
  return encoded;
}

ImmutableBatch batch() {
  ImmutableBatch value{};
  constexpr std::array<uint8_t, 4U> body{{'{', '}', '\n', 0U}};
  (void)value.prepare("arduino-batch", body.data(), 3U, 1U);
  value.set_device_id(device());
  sigap::config::ConfigHash source_hash{};
  source_hash[0U] = 0xA1U;
  (void)value.bind_source(
      device(), boot(), 1U,
      static_cast<uint8_t>(sigap::storage::ArchiveRecordKind::Sample),
      source_hash);
  return value;
}

Ack ack_for(const ImmutableBatch& value) {
  Ack ack{};
  ack.batch_id_size = value.batch_id_size;
  std::memcpy(ack.batch_id.data(), value.batch_id.data(), value.batch_id_size);
  ack.body_hash = value.body_hash;
  ack.record_count = value.record_count;
  ack.device_id = value.device_id;
  ack.stored_count = value.record_count;
  ack.status = AckStatus::Durable;
  return ack;
}

void test_arduino_archive_failed_copy_restarts_from_marked_source() {
  fs::FS disk{};
  LittleFsArchiveStore store("/sigap/archive.bin");
  TEST_ASSERT_TRUE(store.begin(disk, 64U * 1024U));
  const auto first = record(1U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  const std::array<uint8_t, 3U> corrupt{{0xC1U, 0xC2U, 0xC3U}};
  disk.append_raw("/sigap/archive.bin", corrupt.data(), corrupt.size());

  disk.short_write_after(1U, 1U);
  TEST_ASSERT_FALSE(store.recover());
  TEST_ASSERT_TRUE(disk.exists("/sigap/archive.bin.tail0"));
  TEST_ASSERT_TRUE(disk.exists("/sigap/archive.bin.recover"));

  disk.clear_faults();
  LittleFsArchiveStore restarted("/sigap/archive.bin");
  TEST_ASSERT_TRUE(restarted.begin(disk, 64U * 1024U));
  TEST_ASSERT_TRUE(restarted.contains(first.identity, first.hash));
  const auto restored = disk.bytes("/sigap/archive.bin");
  TEST_ASSERT_EQUAL_UINT32(first.size, restored.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.bytes.data(), restored.data(), first.size);
  TEST_ASSERT_FALSE(disk.exists("/sigap/archive.bin.recover"));
}

void test_arduino_archive_recovery_rejects_failed_rename_and_short_read() {
  fs::FS disk{};
  LittleFsArchiveStore store("/sigap/archive.bin");
  TEST_ASSERT_TRUE(store.begin(disk, 64U * 1024U));
  const auto first = record(2U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  const uint8_t corrupt = 0xEEU;
  disk.append_raw("/sigap/archive.bin", &corrupt, 1U);
  disk.fail_next_rename();
  TEST_ASSERT_FALSE(store.recover());
  TEST_ASSERT_TRUE(disk.exists("/sigap/archive.bin"));
  TEST_ASSERT_FALSE(disk.exists("/sigap/archive.bin.tail0"));

  disk.short_read_once(1U);
  TEST_ASSERT_FALSE(store.contains(first.identity, first.hash));
  disk.clear_faults();
  TEST_ASSERT_TRUE(store.recover());
}

void test_arduino_archive_reclaim_does_not_resurrect_tail_after_restart() {
  fs::FS disk{};
  LittleFsArchiveStore store("/sigap/archive.bin");
  TEST_ASSERT_TRUE(store.begin(disk, 64U * 1024U));
  const auto first = record(3U);
  const auto second = record(4U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(second)));
  const uint8_t corrupt = 0xEFU;
  disk.append_raw("/sigap/archive.bin", &corrupt, 1U);
  TEST_ASSERT_TRUE(store.recover());
  TEST_ASSERT_TRUE(disk.exists("/sigap/archive.bin.tail0"));
  TEST_ASSERT_TRUE(store.reclaim_pending(first.identity, first.hash));

  LittleFsArchiveStore restarted("/sigap/archive.bin");
  TEST_ASSERT_TRUE(restarted.begin(disk, 64U * 1024U));
  TEST_ASSERT_FALSE(restarted.contains(first.identity, first.hash));
  TEST_ASSERT_TRUE(restarted.contains(second.identity, second.hash));
  TEST_ASSERT_TRUE(disk.exists("/sigap/archive.bin.tail0"));
}

void test_arduino_ack_recovery_preserves_valid_prefix_after_failed_copy() {
  fs::FS disk{};
  FsAckJournalStore journal;
  TEST_ASSERT_TRUE(journal.begin(disk, "/sigap/acks.bin"));
  const auto value = batch();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(value)));
  const std::array<uint8_t, 2U> corrupt{{0xA4U, 0xA5U}};
  disk.append_raw("/sigap/acks.bin", corrupt.data(), corrupt.size());
  disk.short_write_after(1U, 1U);
  TEST_ASSERT_FALSE(journal.recover());
  TEST_ASSERT_TRUE(disk.exists("/sigap/acks.bin.tail"));
  TEST_ASSERT_TRUE(disk.exists("/sigap/acks.bin.recover"));

  disk.clear_faults();
  FsAckJournalStore restarted;
  TEST_ASSERT_TRUE(restarted.begin(disk, "/sigap/acks.bin"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(restarted.apply_ack(ack_for(value))));
  TEST_ASSERT_TRUE(restarted.checkpoint());
  TEST_ASSERT_EQUAL_UINT32(1U, restarted.reclaim());

  FsAckJournalStore final_instance;
  TEST_ASSERT_TRUE(final_instance.begin(disk, "/sigap/acks.bin"));
  TEST_ASSERT_EQUAL_UINT32(0U, final_instance.reclaim());
}

void test_arduino_ack_open_failure_and_marker_are_observable() {
  fs::FS disk{};
  FsAckJournalStore journal;
  TEST_ASSERT_TRUE(journal.begin(disk, "/sigap/acks.bin"));
  const auto value = batch();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(value)));
  disk.fail_next_open_for("/sigap/acks.bin");
  TEST_ASSERT_FALSE(journal.recover());
  disk.clear_faults();
  FsAckJournalStore restarted;
  TEST_ASSERT_TRUE(restarted.begin(disk, "/sigap/acks.bin"));
  disk.short_write_once(1U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Retryable),
                        static_cast<int>(restarted.apply_ack(ack_for(value))));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_arduino_archive_failed_copy_restarts_from_marked_source);
  RUN_TEST(test_arduino_archive_recovery_rejects_failed_rename_and_short_read);
  RUN_TEST(test_arduino_archive_reclaim_does_not_resurrect_tail_after_restart);
  RUN_TEST(test_arduino_ack_recovery_preserves_valid_prefix_after_failed_copy);
  RUN_TEST(test_arduino_ack_open_failure_and_marker_are_observable);
  return UNITY_END();
}
