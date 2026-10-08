#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <unity.h>

#include "storage/archive_codec.h"
#include "storage/archive_store.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::storage;
using namespace sigap::telemetry_archive;

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
  value.config_version = 4U;
  value.hash[0] = 0x44U;
  return value;
}

ImmutableRecordBytes sample(uint64_t sequence) {
  SampleRecord value{};
  value.header.device_id = device();
  value.header.boot_id = boot();
  value.header.sequence = sequence;
  value.header.monotonic_us = sequence * 1000U;
  value.header.sensor = SensorId::Mq4;
  value.header.stream = StreamId::Organic;
  value.header.kind = SampleKind::MqAdc;
  value.quality = SampleQuality::Uncalibrated;
  value.reason = SampleReason::ProfileMissing;
  value.raw.present = true;
  value.raw.adc_counts = 1200U;
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(value, encoded));
  return encoded;
}

ImmutableRecordBytes boot_metadata(uint64_t sequence) {
  const ArchiveIdentity identity{device(), boot(), sequence,
                                 ArchiveRecordKind::Metadata};
  ImmutableRecordBytes encoded{};
  PayloadWriter writer{nullptr, 0U};
  TEST_ASSERT_TRUE(RecordCodec::begin(identity, encoded, writer));
  writer = PayloadWriter(encoded.bytes.data() + kArchiveHeaderBytes,
                         encoded.bytes.size() - kArchiveHeaderBytes -
                             kArchiveTrailerBytes);
  ConfigRef config = config_ref();
  constexpr std::string_view version{"sigap-task6"};
  TEST_ASSERT_TRUE(writer.u8(static_cast<uint8_t>(ArchiveMetadataType::Boot)));
  TEST_ASSERT_TRUE(writer.u64(1234U));
  TEST_ASSERT_TRUE(writer.u8(0U));
  TEST_ASSERT_TRUE(writer.i64(0));
  TEST_ASSERT_TRUE(writer.u8(static_cast<uint8_t>(TimeQuality::Unsynced)));
  TEST_ASSERT_TRUE(writer.u32(config.schema_version));
  TEST_ASSERT_TRUE(writer.u64(config.config_version));
  TEST_ASSERT_TRUE(writer.u8(1U));
  TEST_ASSERT_TRUE(writer.bytes(config.hash.data(), config.hash.size()));
  TEST_ASSERT_TRUE(writer.u16(0U));
  TEST_ASSERT_TRUE(writer.u8(static_cast<uint8_t>(version.size())));
  TEST_ASSERT_TRUE(writer.bytes(reinterpret_cast<const uint8_t*>(version.data()),
                                version.size()));
  TEST_ASSERT_TRUE(writer.u8(1U));
  TEST_ASSERT_TRUE(RecordCodec::finish(encoded, writer));
  return encoded;
}

ImmutableRecordBytes sht31_partial(uint64_t sequence) {
  SampleRecord value{};
  value.header.device_id = device();
  value.header.boot_id = boot();
  value.header.sequence = sequence;
  value.header.monotonic_us = sequence * 1000U;
  value.header.sensor = SensorId::Sht31_44;
  value.header.stream = StreamId::Organic;
  value.header.kind = SampleKind::Sht31;
  value.quality = SampleQuality::CrcError;
  value.reason = SampleReason::CrcError;
  value.raw.present = true;
  value.raw.temperature_centi_c = 1250;
  value.raw.humidity_centi_pct = 4550;
  value.raw.temperature_crc_valid = true;
  value.raw.humidity_crc_valid = false;
  value.converted.primary_available = true;
  value.converted.primary_value = 12.5F;
  value.converted.secondary_available = false;
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(value, encoded));
  return encoded;
}

Ack ack_for(const ImmutableBatch& batch) {
  Ack ack{};
  ack.batch_id_size = batch.batch_id_size;
  std::memcpy(ack.batch_id.data(), batch.batch_id.data(), batch.batch_id_size);
  ack.body_hash = batch.body_hash;
  ack.record_count = batch.record_count;
  ack.stored_count = batch.record_count;
  ack.device_id = batch.device_id;
  ack.status = AckStatus::Durable;
  return ack;
}

ImmutableBatch manifest_batch(uint64_t sequence) {
  ImmutableBatch batch{};
  char id[32]{};
  const int written = std::snprintf(id, sizeof(id), "batch-%llu",
                                    static_cast<unsigned long long>(sequence));
  TEST_ASSERT_TRUE(written > 0);
  const std::array<uint8_t, 3U> body{{0xA1U, static_cast<uint8_t>(sequence), 0x5AU}};
  TEST_ASSERT_TRUE(batch.prepare(std::string_view(id, static_cast<std::size_t>(written)),
                                 body.data(), body.size(), 1U));
  batch.set_device_id(device());
  return batch;
}

void test_runtime_builds_json_batch_and_persists_pending_ack() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  const auto encoded = sample(7U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  TEST_ASSERT_TRUE(runtime.pending());
  TEST_ASSERT_EQUAL_UINT32(1U, batches.size());
  TEST_ASSERT_EQUAL_UINT32(1U, journal.size());
  const auto body = batches.at(0U)->exact_body();
  TEST_ASSERT_TRUE(body.find("{\"schema_version\":3") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"samples\":[") != std::string_view::npos);
}

void test_invalid_ack_does_not_reclaim_archive_record() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  const auto encoded = sample(9U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  auto invalid = ack_for(runtime.batch());
  invalid.status = AckStatus::SummaryV2;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Invalid),
                        static_cast<int>(runtime.apply_ack(invalid)));
  ImmutableRecordBytes replay{};
  TEST_ASSERT_TRUE(archive.peek_pending(replay));
}

void test_valid_ack_checkpoints_before_reclaim() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  const auto encoded = sample(11U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(runtime.apply_ack(ack_for(runtime.batch()))));
  ImmutableRecordBytes replay{};
  TEST_ASSERT_FALSE(archive.peek_pending(replay));
}

void test_metadata_decoder_and_batch_builder_preserve_boot_provenance() {
  const auto encoded = boot_metadata(13U);
  DecodedMetadataPayload decoded{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TypedDecodeStatus::Decoded),
                        static_cast<int>(RecordCodec::decode_metadata(encoded, decoded)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ArchiveMetadataType::Boot),
                        static_cast<int>(decoded.type));
  TEST_ASSERT_EQUAL_UINT64(1234U, decoded.monotonic_us);
  TEST_ASSERT_EQUAL_UINT8(1U, decoded.reset_reason);
  TEST_ASSERT_EQUAL_STRING("sigap-task6", decoded.firmware_version.data());

  BatchJsonBuilder builder{};
  ImmutableBatch batch{};
  TEST_ASSERT_TRUE(builder.build(encoded, batch));
  TEST_ASSERT_TRUE(batch.exact_body().find("\"record_id\":\"11000000000000000000000000000000/22000000000000000000000000000000/13\"") !=
                  std::string_view::npos);
  TEST_ASSERT_TRUE(batch.exact_body().find("\"metadata_type\":\"BOOT\"") !=
                  std::string_view::npos);
  TEST_ASSERT_TRUE(batch.exact_body().find("\"firmware_version\":\"sigap-task6\"") !=
                  std::string_view::npos);
}

void test_sample_batch_preserves_partial_evidence_and_null_conversion() {
  BatchJsonBuilder builder{};
  ImmutableBatch batch{};
  TEST_ASSERT_TRUE(builder.build(sht31_partial(15U), batch));
  const auto body = batch.exact_body();
  TEST_ASSERT_TRUE(body.find("\"temperature_ticks\":1250") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"humidity_crc_valid\":false") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"converted\":null") != std::string_view::npos);
}

void test_ack_replay_is_bound_to_acked_manifest() {
  MemoryAckJournalStore<4U> journal{};
  const auto first = manifest_batch(1U);
  const auto second = manifest_batch(2U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(second)));

  const Ack first_ack = ack_for(first);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.apply_ack(first_ack)));
  TEST_ASSERT_TRUE(journal.find_manifest(first_ack) != nullptr);
  TEST_ASSERT_TRUE(journal.find_manifest(ack_for(second)) != nullptr);
  TEST_ASSERT_TRUE(journal.checkpoint());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.reclaim(first_ack)));

  // Replaying A cannot reclaim or remove B.  The exact B manifest remains
  // pending and is still discoverable by its own identity/hash.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::NotFound),
                        static_cast<int>(journal.reclaim(first_ack)));
  TEST_ASSERT_EQUAL_UINT32(1U, journal.size());
  TEST_ASSERT_TRUE(journal.find_manifest(ack_for(second)) != nullptr);
}

void test_ack_journal_compacts_after_more_than_cache_capacity() {
  MemoryAckJournalStore<16U> journal{};
  for (uint64_t sequence = 1U; sequence <= 40U; ++sequence) {
    const auto batch = manifest_batch(sequence);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                          static_cast<int>(journal.append_pending(batch)));
    const auto ack = ack_for(batch);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                          static_cast<int>(journal.apply_ack(ack)));
    TEST_ASSERT_TRUE(journal.checkpoint());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                          static_cast<int>(journal.reclaim(ack)));
  }
  TEST_ASSERT_EQUAL_UINT32(0U, journal.size());
}

void test_replayed_ack_cannot_reclaim_next_pending_record() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  const auto first = sample(71U);
  const auto second = sample(72U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(first)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(second)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  const Ack first_ack = ack_for(runtime.batch());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(runtime.apply_ack(first_ack)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::NotFound),
                        static_cast<int>(runtime.apply_ack(first_ack)));
  ImmutableRecordBytes replay{};
  TEST_ASSERT_TRUE(archive.peek_pending(replay));
  TEST_ASSERT_EQUAL_UINT64(72U, replay.identity.sequence);
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_runtime_builds_json_batch_and_persists_pending_ack);
  RUN_TEST(test_invalid_ack_does_not_reclaim_archive_record);
  RUN_TEST(test_valid_ack_checkpoints_before_reclaim);
  RUN_TEST(test_metadata_decoder_and_batch_builder_preserve_boot_provenance);
  RUN_TEST(test_sample_batch_preserves_partial_evidence_and_null_conversion);
  RUN_TEST(test_ack_replay_is_bound_to_acked_manifest);
  RUN_TEST(test_ack_journal_compacts_after_more_than_cache_capacity);
  RUN_TEST(test_replayed_ack_cannot_reclaim_next_pending_record);
  return UNITY_END();
}
