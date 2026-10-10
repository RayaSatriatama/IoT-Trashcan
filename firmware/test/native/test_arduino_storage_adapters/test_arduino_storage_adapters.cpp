#define ARDUINO 1

#include <array>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include "storage/archive_media.h"
#include "storage/sd_probe_path.h"
#include "storage/media_migration.h"
#include "network/device_control_journal.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::storage;
using namespace sigap::telemetry_archive;
using namespace sigap::network;

DeviceId device(uint8_t value) {
  DeviceId result{};
  result.present = true;
  result.bytes[0U] = value;
  return result;
}

BootId boot(uint8_t value) {
  BootId result{};
  result.present = true;
  result.bytes[0U] = value;
  return result;
}

ImmutableRecordBytes record(uint64_t sequence) {
  SampleRecord sample{};
  sample.header.device_id = device(0x41U);
  sample.header.boot_id = boot(0x52U);
  sample.header.sequence = sequence;
  sample.header.sensor = SensorId::Mq4;
  sample.header.stream = StreamId::Organic;
  sample.header.kind = SampleKind::MqAdc;
  sample.header.monotonic_us = sequence * 1000U;
  sample.quality = SampleQuality::Timeout;
  sample.reason = SampleReason::Timeout;
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(sample, encoded));
  return encoded;
}

ImmutableBatch batch(const char* id, uint64_t source_sequence) {
  ImmutableBatch result{};
  constexpr std::array<uint8_t, 7U> body{{'{', '"', 'x', '"', ':', '1', '}'}};
  TEST_ASSERT_TRUE(result.prepare(id, body.data(), body.size(), 1U));
  result.set_device_id(device(0x41U));
  config::ConfigHash source_hash{};
  source_hash[0U] = static_cast<uint8_t>(source_sequence);
  TEST_ASSERT_TRUE(result.bind_source(
      result.device_id, boot(0x52U), source_sequence,
      static_cast<uint8_t>(ArchiveRecordKind::Result), source_hash));
  return result;
}

Ack ack_for(const ImmutableBatch& value) {
  Ack result{};
  result.batch_id_size = value.batch_id_size;
  std::copy(value.batch_id.begin(), value.batch_id.end(), result.batch_id.begin());
  result.body_hash = value.body_hash;
  result.record_count = value.record_count;
  result.stored_count = value.record_count;
  result.device_id = value.device_id;
  result.status = AckStatus::Durable;
  return result;
}

DeviceControlCommand control_command(const char* id) {
  DeviceControlCommand value{};
  TEST_ASSERT_TRUE(value.command_id.assign(id));
  TEST_ASSERT_TRUE(value.device_id.assign("0000000000000042"));
  TEST_ASSERT_TRUE(value.operator_id.assign("operator-1"));
  TEST_ASSERT_TRUE(value.session_id.assign("session-1"));
  TEST_ASSERT_TRUE(value.sensor_id.assign("mq4"));
  TEST_ASSERT_TRUE(value.operation.assign("commit"));
  TEST_ASSERT_TRUE(value.request_hash.assign(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  value.payload[0] = '{';
  value.payload[1] = '}';
  value.payload_size = 2U;
  value.expires_at_us = 10U;
  return value;
}

DeviceControlResult control_result(const char* id) {
  DeviceControlResult value{};
  value.status = DeviceControlStatus::Committed;
  TEST_ASSERT_TRUE(value.command_id.assign(id));
  TEST_ASSERT_TRUE(value.session_id.assign("session-1"));
  value.payload[0] = '{';
  value.payload[1] = '"';
  value.payload[2] = '1';
  value.payload[3] = '"';
  value.payload[4] = ':';
  value.payload[5] = '1';
  value.payload[6] = '}';
  value.payload_size = 7U;
  return value;
}

void test_target_archive_failed_recovery_copy_is_restart_safe() {
  fs::FS filesystem{};
  LittleFsArchiveStore initial("/sigap/archive.bin");
  TEST_ASSERT_TRUE(initial.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(filesystem.exists("/sigap"));
  const auto first = record(1U);
  const auto second = record(2U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(initial.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(initial.append(second)));

  constexpr std::array<uint8_t, 3U> corrupt{{0xC1U, 0xC2U, 0xC3U}};
  filesystem.append_raw("/sigap/archive.bin", corrupt.data(), corrupt.size());

  // The first write is the recovery marker. The second write is the prefix
  // copy, which is deliberately short so the source remains authoritative.
  filesystem.short_write_after(1U, 13U);
  LittleFsArchiveStore failed("/sigap/archive.bin");
  TEST_ASSERT_FALSE(failed.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(filesystem.exists("/sigap/archive.bin.tail0"));

  filesystem.clear_faults();
  LittleFsArchiveStore restarted("/sigap/archive.bin");
  TEST_ASSERT_TRUE(restarted.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(restarted.contains(first.identity, first.hash));
  TEST_ASSERT_TRUE(restarted.contains(second.identity, second.hash));

  TEST_ASSERT_TRUE(restarted.reclaim_pending(first.identity, first.hash));
  LittleFsArchiveStore after_reclaim("/sigap/archive.bin");
  TEST_ASSERT_TRUE(after_reclaim.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_FALSE(after_reclaim.contains(first.identity, first.hash));
  TEST_ASSERT_TRUE(after_reclaim.contains(second.identity, second.hash));
}

void test_target_archive_reclaims_non_head_probe_record() {
  fs::FS filesystem{};
  LittleFsArchiveStore store("/sigap/archive.bin");
  TEST_ASSERT_TRUE(store.begin(filesystem, 1024U * 1024U));
  const auto first = record(101U);
  const auto second = record(102U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(second)));
  TEST_ASSERT_TRUE(store.reclaim_pending(second.identity, second.hash));

  LittleFsArchiveStore restarted("/sigap/archive.bin");
  TEST_ASSERT_TRUE(restarted.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(restarted.contains(first.identity, first.hash));
  TEST_ASSERT_FALSE(restarted.contains(second.identity, second.hash));
}

void test_target_archive_short_append_recovers_valid_prefix() {
  fs::FS filesystem{};
  LittleFsArchiveStore store("/sigap/archive.bin");
  TEST_ASSERT_TRUE(store.begin(filesystem, 1024U * 1024U));
  const auto first = record(103U);
  const auto second = record(104U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));

  filesystem.short_write_once(13U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Retryable),
                        static_cast<int>(store.append(second)));
  filesystem.clear_faults();

  LittleFsArchiveStore recovered("/sigap/archive.bin");
  TEST_ASSERT_TRUE(recovered.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(recovered.contains(first.identity, first.hash));
  TEST_ASSERT_FALSE(recovered.contains(second.identity, second.hash));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(recovered.append(second)));

  LittleFsArchiveStore restarted("/sigap/archive.bin");
  TEST_ASSERT_TRUE(restarted.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(restarted.contains(first.identity, first.hash));
  TEST_ASSERT_TRUE(restarted.contains(second.identity, second.hash));
}

void test_target_archive_failed_reclaim_preserves_source() {
  fs::FS filesystem{};
  LittleFsArchiveStore store("/sigap/archive.bin");
  TEST_ASSERT_TRUE(store.begin(filesystem, 1024U * 1024U));
  const auto first = record(105U);
  const auto second = record(106U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(second)));

  filesystem.short_write_once(13U);
  TEST_ASSERT_FALSE(store.reclaim_pending(first.identity, first.hash));
  filesystem.clear_faults();

  LittleFsArchiveStore after_copy_failure("/sigap/archive.bin");
  TEST_ASSERT_TRUE(after_copy_failure.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(after_copy_failure.contains(first.identity, first.hash));
  TEST_ASSERT_TRUE(after_copy_failure.contains(second.identity, second.hash));

  filesystem.fail_next_rename();
  TEST_ASSERT_FALSE(after_copy_failure.reclaim_pending(first.identity,
                                                        first.hash));
  LittleFsArchiveStore after_rename_failure("/sigap/archive.bin");
  TEST_ASSERT_TRUE(after_rename_failure.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_TRUE(after_rename_failure.contains(first.identity, first.hash));
  TEST_ASSERT_TRUE(after_rename_failure.contains(second.identity, second.hash));
}

void test_target_archive_empty_after_legitimate_reclaim_keeps_tail_forensic() {
  fs::FS filesystem{};
  LittleFsArchiveStore store("/sigap/archive.bin");
  TEST_ASSERT_TRUE(store.begin(filesystem, 1024U * 1024U));
  const auto only = record(107U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(store.append(only)));

  const std::array<uint8_t, 1U> corrupt{{0xE3U}};
  filesystem.append_raw("/sigap/archive.bin", corrupt.data(), corrupt.size());
  TEST_ASSERT_TRUE(store.recover());
  TEST_ASSERT_TRUE(filesystem.exists("/sigap/archive.bin.tail0"));

  // The verified tail is forensic-only after recovery. Reclaiming the last
  // pending frame must leave an empty, usable primary archive rather than
  // treating that tail as pending data on the next recovery.
  TEST_ASSERT_TRUE(store.reclaim_pending(only.identity, only.hash));
  LittleFsArchiveStore restarted("/sigap/archive.bin");
  TEST_ASSERT_TRUE(restarted.begin(filesystem, 1024U * 1024U));
  TEST_ASSERT_FALSE(restarted.contains(only.identity, only.hash));
  TEST_ASSERT_TRUE(filesystem.exists("/sigap/archive.bin.tail0"));
}

void test_sd_probe_path_is_device_owned_and_bounded() {
  char path[64U]{};
  TEST_ASSERT_TRUE(sigap::storage::build_sd_probe_path(
      0x98A316F5C150ULL, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING("/sigap/task9_sd_probe_16f5c150.bin", path);

  char too_small[8U]{};
  TEST_ASSERT_FALSE(sigap::storage::build_sd_probe_path(
      0x98A316F5C150ULL, too_small, sizeof(too_small)));
  TEST_ASSERT_FALSE(sigap::storage::build_sd_probe_path(
      0x98A316F5C150ULL, nullptr, sizeof(path)));
}

void test_target_batch_and_ack_replay_preserve_exact_bytes_and_binding() {
  fs::FS filesystem{};
  FsImmutableBatchStore batches;
  TEST_ASSERT_TRUE(batches.begin(filesystem, 1024U * 1024U));
  FsAckJournalStore journal;
  TEST_ASSERT_TRUE(journal.begin(filesystem));

  const auto first = batch("batch-a", 11U);
  const auto second = batch("batch-b", 12U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(batches.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(batches.append(second)));
  TEST_ASSERT_TRUE(batches.checkpoint());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(second)));
  TEST_ASSERT_TRUE(journal.checkpoint());

  const auto first_ack = ack_for(first);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.apply_ack(first_ack)));
  TEST_ASSERT_TRUE(journal.checkpoint());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.reclaim(first_ack)));
  ImmutableBatch replay{};
  TEST_ASSERT_TRUE(batches.load_manifest(first_ack, replay));
  TEST_ASSERT_EQUAL_UINT32(first.body_size, replay.body_size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.body.data(), replay.body.data(),
                                first.body_size);
  TEST_ASSERT_TRUE(replay.source.present);
  TEST_ASSERT_EQUAL_UINT64(first.source.sequence, replay.source.sequence);
}

void test_target_ack_restart_does_not_reclaim_wrong_pending_batch() {
  fs::FS filesystem{};
  FsAckJournalStore journal;
  TEST_ASSERT_TRUE(journal.begin(filesystem));
  const auto first = batch("batch-a", 21U);
  const auto second = batch("batch-b", 22U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(second)));
  TEST_ASSERT_TRUE(journal.checkpoint());

  auto first_ack = ack_for(first);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.apply_ack(first_ack)));
  TEST_ASSERT_TRUE(journal.checkpoint());

  FsAckJournalStore restarted;
  TEST_ASSERT_TRUE(restarted.begin(filesystem));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(restarted.reclaim(first_ack)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::NotFound),
                        static_cast<int>(restarted.apply_ack(first_ack)));

  const auto second_ack = ack_for(second);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(restarted.apply_ack(second_ack)));
}

void test_target_ack_short_frame_recovery_preserves_valid_prefix() {
  fs::FS filesystem{};
  FsAckJournalStore journal;
  TEST_ASSERT_TRUE(journal.begin(filesystem));
  const auto value = batch("batch-short", 31U);

  filesystem.short_write_once(13U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Retryable),
                        static_cast<int>(journal.append_pending(value)));
  filesystem.clear_faults();

  FsAckJournalStore restarted;
  TEST_ASSERT_TRUE(restarted.begin(filesystem));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(restarted.append_pending(value)));
  TEST_ASSERT_TRUE(restarted.checkpoint());

  // A successful retry must clear recovery authority before new appends. A
  // later restart must not replay the old zero-length quarantine over this
  // newly committed frame.
  FsAckJournalStore after_append_restart;
  TEST_ASSERT_TRUE(after_append_restart.begin(filesystem));
  TEST_ASSERT_TRUE(after_append_restart.find_manifest(ack_for(value)) != nullptr);
}

void test_target_batch_marker_clears_before_new_append() {
  fs::FS filesystem{};
  FsImmutableBatchStore initial;
  TEST_ASSERT_TRUE(initial.begin(filesystem, 1024U * 1024U));
  const auto first = batch("batch-tail", 35U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(initial.append(first)));
  constexpr std::array<uint8_t, 2U> corrupt{{0xE1U, 0xE2U}};
  filesystem.append_raw("/sigap/batches.bin", corrupt.data(), corrupt.size());

  filesystem.short_write_after(1U, 7U);
  FsImmutableBatchStore failed;
  TEST_ASSERT_FALSE(failed.begin(filesystem, 1024U * 1024U));
  filesystem.clear_faults();

  FsImmutableBatchStore restarted;
  TEST_ASSERT_TRUE(restarted.begin(filesystem, 1024U * 1024U));
  const auto second = batch("batch-next", 36U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(restarted.append(second)));

  FsImmutableBatchStore after_append_restart;
  TEST_ASSERT_TRUE(after_append_restart.begin(filesystem, 1024U * 1024U));
  Ack ack = ack_for(second);
  ImmutableBatch replay{};
  TEST_ASSERT_TRUE(after_append_restart.load_manifest(ack, replay));
  TEST_ASSERT_EQUAL_UINT32(second.body_size, replay.body_size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(second.body.data(), replay.body.data(),
                                second.body_size);
}

void test_target_migration_receipt_survives_restart_boundary() {
  fs::FS filesystem{};
  FsMigrationReceiptJournal journal("/sigap/archive.migration");
  TEST_ASSERT_TRUE(journal.begin(filesystem));
  MigrationReceipt receipt{};
  receipt.identity = record(41U).identity;
  receipt.hash[0U] = 0xA1U;
  receipt.source = ArchiveMedia::LittleFs;
  receipt.destination = ArchiveMedia::Sd;
  receipt.checkpoint_generation = 9U;
  receipt.phase = MigrationPhase::Checkpointed;
  receipt.durable = true;
  TEST_ASSERT_TRUE(journal.persist(receipt));

  FsMigrationReceiptJournal restarted("/sigap/archive.migration");
  TEST_ASSERT_TRUE(restarted.begin(filesystem));
  MigrationReceipt loaded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationReceiptLoad::Found),
                        static_cast<int>(restarted.load(loaded)));
  TEST_ASSERT_EQUAL_UINT64(9U, loaded.checkpoint_generation);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(receipt.hash.data(), loaded.hash.data(),
                                receipt.hash.size());
  TEST_ASSERT_TRUE(restarted.clear(loaded));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(MigrationReceiptLoad::NotFound),
                        static_cast<int>(restarted.load(loaded)));
}

void test_target_device_control_journal_replays_exact_result_after_restart() {
  fs::FS filesystem{};
  DeviceControlCommand expected_command = control_command("control-a");
  DeviceControlResult expected_result = control_result("control-a");
  FsDeviceControlResultJournal journal;
  TEST_ASSERT_TRUE(journal.begin(filesystem, "/sigap/device-control.result"));
  TEST_ASSERT_TRUE(journal.recover());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceControlJournalState::Empty),
                        static_cast<int>(journal.state()));
  TEST_ASSERT_TRUE(journal.save(expected_command, expected_result));

  FsDeviceControlResultJournal restarted;
  TEST_ASSERT_TRUE(restarted.begin(filesystem, "/sigap/device-control.result"));
  TEST_ASSERT_TRUE(restarted.recover());
  DeviceControlCommand recovered_command{};
  DeviceControlResult recovered_result{};
  TEST_ASSERT_TRUE(restarted.load(recovered_command, recovered_result));
  TEST_ASSERT_EQUAL_STRING(expected_command.command_id.bytes.data(),
                           recovered_command.command_id.bytes.data());
  TEST_ASSERT_EQUAL_UINT32(expected_result.payload_size,
                           recovered_result.payload_size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_result.payload.data(),
                                recovered_result.payload.data(),
                                expected_result.payload_size);
  TEST_ASSERT_TRUE(restarted.clear());
}

void test_target_device_control_journal_fault_is_not_empty() {
  fs::FS filesystem{};
  FsDeviceControlResultJournal journal;
  TEST_ASSERT_TRUE(journal.begin(filesystem, "/sigap/device-control.result"));
  filesystem.short_write_once(3U);
  TEST_ASSERT_FALSE(journal.save(control_command("control-b"),
                                 control_result("control-b")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceControlJournalState::IoError),
                        static_cast<int>(journal.state()));
  FsDeviceControlResultJournal restarted;
  TEST_ASSERT_FALSE(restarted.begin(filesystem, "/sigap/device-control.result"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceControlJournalState::Corrupt),
                        static_cast<int>(restarted.state()));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_target_archive_failed_recovery_copy_is_restart_safe);
  RUN_TEST(test_target_archive_reclaims_non_head_probe_record);
  RUN_TEST(test_target_archive_short_append_recovers_valid_prefix);
  RUN_TEST(test_target_archive_failed_reclaim_preserves_source);
  RUN_TEST(test_target_archive_empty_after_legitimate_reclaim_keeps_tail_forensic);
  RUN_TEST(test_sd_probe_path_is_device_owned_and_bounded);
  RUN_TEST(test_target_batch_and_ack_replay_preserve_exact_bytes_and_binding);
  RUN_TEST(test_target_ack_restart_does_not_reclaim_wrong_pending_batch);
  RUN_TEST(test_target_ack_short_frame_recovery_preserves_valid_prefix);
  RUN_TEST(test_target_batch_marker_clears_before_new_append);
  RUN_TEST(test_target_migration_receipt_survives_restart_boundary);
  RUN_TEST(test_target_device_control_journal_replays_exact_result_after_restart);
  RUN_TEST(test_target_device_control_journal_fault_is_not_empty);
  return UNITY_END();
}
