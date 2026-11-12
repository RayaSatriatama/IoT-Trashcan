#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <unity.h>

#include "acquisition/target_protocol.h"
#include "storage/archive_codec.h"
#include "storage/archive_store.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"
#include "debug/types.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::storage;
using namespace sigap::telemetry_archive;

struct StorageSink final : debug::IEventSink {
  bool accept{true};
  unsigned attempts{};
  std::array<debug::EventInput, 16U> events{};
  std::size_t count{};
  bool emit(const debug::EventInput& event) noexcept override {
    ++attempts;
    if (!accept) return false;
    if (count < events.size()) events[count++] = event;
    return true;
  }
};
struct StorageClock { uint64_t now{456U}; };
uint64_t read_storage_clock(void* context) noexcept {
  return static_cast<StorageClock*>(context)->now;
}

struct ManifestMismatchBatchStore final : IImmutableBatchStore {
  MemoryImmutableBatchStore<8U> inner{256U * 1024U};
  BatchStoreStatus append(const ImmutableBatch& batch) noexcept override {
    return inner.append(batch);
  }
  bool checkpoint() noexcept override { return inner.checkpoint(); }
  bool recover() noexcept override { return inner.recover(); }
  bool load_manifest(const Ack& ack, ImmutableBatch& output) noexcept override {
    if (!inner.load_manifest(ack, output)) return false;
    ++output.record_count;
    return true;
  }
  uint64_t pending_bytes() const noexcept override { return inner.pending_bytes(); }
  uint64_t quota_bytes() const noexcept override { return inner.quota_bytes(); }
};

struct FalseReclaimArchive final : IArchiveStore {
  explicit FalseReclaimArchive(bool source_remains) noexcept
      : source_remains_(source_remains) {}
  MemoryArchiveStore<8U> inner{256U * 1024U};
  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
    return inner.append(record);
  }
  AppendStatus append_gap(const GapRange& range) noexcept override {
    return inner.append_gap(range);
  }
  bool checkpoint() noexcept override { return inner.checkpoint(); }
  bool recover() noexcept override { return inner.recover(); }
  uint64_t pending_bytes() const noexcept override { return inner.pending_bytes(); }
  uint64_t pending_quota_bytes() const noexcept override {
    return inner.pending_quota_bytes();
  }
  ArchiveHealth health() const noexcept override { return inner.health(); }
  bool peek_pending(ImmutableRecordBytes& output) noexcept override {
    return inner.peek_pending(output);
  }
  bool reclaim_pending(const ArchiveIdentity& identity,
                       const config::ConfigHash& hash) noexcept override {
    ++reclaim_calls;
    (void)identity;
    (void)hash;
    return false;
  }
  bool contains(const ArchiveIdentity& identity,
                const config::ConfigHash& hash) noexcept override {
    ++contains_calls;
    return source_remains_ && inner.contains(identity, hash);
  }
  std::size_t reclaim_calls{};
  std::size_t contains_calls{};
 private:
  bool source_remains_;
};

struct RetryOnceAckJournal final {
  MemoryAckJournalStore<8U> inner{};
  AckPersistStatus append_pending(const ImmutableBatch& batch) noexcept {
    return inner.append_pending(batch);
  }
  AckPersistStatus apply_ack(const Ack& ack) noexcept {
    return inner.apply_ack(ack);
  }
  bool checkpoint() noexcept { return inner.checkpoint(); }
  AckPersistStatus reclaim(const Ack& ack) noexcept {
    if (retry_reclaim_once) {
      retry_reclaim_once = false;
      return AckPersistStatus::Retryable;
    }
    return inner.reclaim(ack);
  }
  bool retry_reclaim_once{true};
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
  std::array<uint8_t, 6U> frame{{0x30U, 0x39U, 0U, 0xd4U, 0x31U, 0U}};
  frame[2] = sht31_crc8(frame.data(), 2U);
  frame[5] = static_cast<uint8_t>(sht31_crc8(frame.data() + 3U, 2U) ^ 1U);
  const auto decoded = decode_sht31_frame(frame);
  value.raw.sht31_ticks_available = true;
  value.raw.sht31_temperature_ticks = static_cast<uint16_t>((frame[0] << 8U) | frame[1]);
  value.raw.sht31_humidity_ticks = static_cast<uint16_t>((frame[3] << 8U) | frame[4]);
  value.raw.temperature_centi_c = decoded.temperature_centi_c;
  value.raw.humidity_centi_pct = decoded.humidity_centi_pct;
  value.raw.temperature_crc_valid = decoded.temperature_crc_valid;
  value.raw.humidity_crc_valid = decoded.humidity_crc_valid;
  value.converted.primary_available = true;
  value.converted.primary_kind = ConvertedKind::Celsius;
  value.converted.primary_value = static_cast<float>(decoded.temperature_centi_c) / 100.0F;
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
  StorageSink sink{};
  StorageClock clock{};
  runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
  const auto encoded = sample(9U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  auto invalid = ack_for(runtime.batch());
  invalid.status = AckStatus::SummaryV2;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Invalid),
                        static_cast<int>(runtime.apply_ack(invalid)));
  TEST_ASSERT_EQUAL_UINT32(0U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.attempts);
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
  TEST_ASSERT_TRUE(body.find("\"temperature_ticks\":12345") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"humidity_ticks\":54321") != std::string_view::npos);
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

void test_ack_recovery_reclaims_source_after_runtime_restart() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>>
      first_runtime(&archive, &batches, &journal);
  const auto encoded = sample(81U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(first_runtime.poll()));
  const Ack ack = ack_for(first_runtime.batch());

  // The second runtime represents a reboot after the manifest and ACK are
  // durable but before the source archive frame was reclaimed.
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>>
      restarted_runtime(&archive, &batches, &journal);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(restarted_runtime.apply_ack(ack)));
  ImmutableRecordBytes replay{};
  TEST_ASSERT_FALSE(archive.peek_pending(replay));
  TEST_ASSERT_EQUAL_UINT32(0U, journal.size());
}

void test_ack_restart_uses_exact_persisted_batch_not_current_serializer() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  const auto encoded = sample(91U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));

  // Represents a batch produced by an older serializer. Its exact bytes are
  // intentionally different from BatchJsonBuilder's current output.
  ImmutableBatch persisted{};
  constexpr std::string_view historical_body{
      "{\"schema_version\":3,\"historical_serializer\":true}"};
  TEST_ASSERT_TRUE(persisted.prepare(
      "historical-batch-91",
      reinterpret_cast<const uint8_t*>(historical_body.data()),
      historical_body.size(), 1U));
  persisted.set_device_id(encoded.identity.device_id);
  TEST_ASSERT_TRUE(persisted.bind_source(
      encoded.identity.device_id, encoded.identity.boot_id,
      encoded.identity.sequence, static_cast<uint8_t>(encoded.identity.kind),
      encoded.hash));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(batches.append(persisted)));
  TEST_ASSERT_TRUE(batches.checkpoint());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(persisted)));

  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>>
      restarted_runtime(&archive, &batches, &journal);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(
                            restarted_runtime.apply_ack(ack_for(persisted))));
  ImmutableRecordBytes replay{};
  TEST_ASSERT_FALSE(archive.peek_pending(replay));
  TEST_ASSERT_EQUAL_UINT32(0U, journal.size());
}

void test_ack_persisted_manifest_validates_multirecord_count_and_reclaims_one_source() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  StorageSink sink{};
  StorageClock clock{};
  const auto encoded = sample(92U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));

  ImmutableBatch persisted{};
  constexpr std::string_view historical_body{
      "{\"schema_version\":3,\"historical_serializer\":true}"};
  constexpr uint16_t kPersistedRecordCount = 3U;
  TEST_ASSERT_TRUE(persisted.prepare(
      "historical-multirecord-92",
      reinterpret_cast<const uint8_t*>(historical_body.data()),
      historical_body.size(), kPersistedRecordCount));
  persisted.set_device_id(encoded.identity.device_id);
  TEST_ASSERT_TRUE(persisted.bind_source(
      encoded.identity.device_id, encoded.identity.boot_id,
      encoded.identity.sequence, static_cast<uint8_t>(encoded.identity.kind),
      encoded.hash));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(batches.append(persisted)));
  TEST_ASSERT_TRUE(batches.checkpoint());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(persisted)));

  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>>
      restarted_runtime(&archive, &batches, &journal);
  restarted_runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
  Ack wrong_count = ack_for(persisted);
  --wrong_count.record_count;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Invalid),
                        static_cast<int>(restarted_runtime.apply_ack(wrong_count)));
  TEST_ASSERT_EQUAL_UINT32(0U, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(1U, archive.size());
  TEST_ASSERT_EQUAL_UINT32(1U, journal.size());

  const Ack valid_ack = ack_for(persisted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(restarted_runtime.apply_ack(valid_ack)));
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_UINT16(32U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT32(kPersistedRecordCount, sink.events[0].arg1);
  TEST_ASSERT_EQUAL_UINT16(32U, sink.events[1].event_code);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[1].arg1);
  TEST_ASSERT_EQUAL_UINT64(clock.now, sink.events[0].monotonic_us);
  TEST_ASSERT_EQUAL_UINT32(0U, archive.size());
  TEST_ASSERT_EQUAL_UINT32(0U, journal.size());
}

void test_storage_ack_and_reclaim_events_follow_actual_durable_operations() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  StorageSink sink{};
  StorageClock clock{};
  runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
  const auto encoded = sample(101U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(encoded)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  const Ack ack = ack_for(runtime.batch());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(runtime.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_UINT16(32U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg1);
  TEST_ASSERT_EQUAL_UINT16(32U, sink.events[1].event_code);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[1].arg1);
  TEST_ASSERT_EQUAL_UINT64(clock.now, sink.events[0].monotonic_us);
  TEST_ASSERT_EQUAL_UINT32(0U, archive.size());

  const unsigned calls_before = sink.attempts;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::NotFound),
                        static_cast<int>(runtime.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(calls_before, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
}

void test_refusing_storage_sink_cannot_change_ack_result_or_archive() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  StorageSink sink{};
  sink.accept = false;
  StorageClock clock{};
  runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(sample(102U))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  const Ack ack = ack_for(runtime.batch());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(runtime.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(2U, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(0U, archive.size());
}

void test_ack_checkpoint_failure_emits_no_false_storage_completion() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  StorageSink sink{};
  StorageClock clock{};
  runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(sample(103U))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  journal.set_checkpoint_failure(true);
  const Ack ack = ack_for(runtime.batch());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Retryable),
                        static_cast<int>(runtime.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(0U, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(1U, archive.size());
}

void test_manifest_mismatch_emits_no_false_storage_completion() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  ManifestMismatchBatchStore batches{};
  MemoryAckJournalStore<8U> journal{};
  ArchiveBatchRuntime<ManifestMismatchBatchStore, MemoryAckJournalStore<8U>> runtime(
      &archive, &batches, &journal);
  StorageSink sink{};
  StorageClock clock{};
  runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(sample(104U))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  const Ack ack = ack_for(runtime.batch());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Retryable),
                        static_cast<int>(runtime.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(0U, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(1U, archive.size());
}

void test_false_archive_reclaim_and_contains_variants_never_claim_reclaimed() {
  for (const bool source_remains : std::array<bool, 2U>{{true, false}}) {
    FalseReclaimArchive archive(source_remains);
    MemoryImmutableBatchStore<8U> batches(256U * 1024U);
    MemoryAckJournalStore<8U> journal{};
    ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, MemoryAckJournalStore<8U>> runtime(
        &archive, &batches, &journal);
    StorageSink sink{};
    StorageClock clock{};
    runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                          static_cast<int>(archive.append(sample(source_remains ? 105U : 106U))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                          static_cast<int>(runtime.poll()));
    if (!source_remains) {
      const auto& source = runtime.batch().source;
      const ArchiveIdentity identity{source.device_id, source.boot_id,
                                     source.sequence,
                                     static_cast<ArchiveRecordKind>(source.record_kind)};
      TEST_ASSERT_TRUE(archive.inner.reclaim_pending(identity, source.record_hash));
    }
    const Ack ack = ack_for(runtime.batch());
    const auto expected = source_remains ? AckPersistStatus::Retryable
                                         : AckPersistStatus::Durable;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected),
                          static_cast<int>(runtime.apply_ack(ack)));
    TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
    TEST_ASSERT_EQUAL_UINT16(32U, sink.events[0].event_code);
    TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
    TEST_ASSERT_EQUAL_UINT32(1U, archive.reclaim_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, archive.contains_calls);
    TEST_ASSERT_EQUAL_UINT32(source_remains ? 1U : 0U, archive.inner.size());
  }
}

void test_archive_reclaim_is_reported_when_later_journal_reclaim_retries() {
  MemoryArchiveStore<8U> archive(256U * 1024U);
  MemoryImmutableBatchStore<8U> batches(256U * 1024U);
  RetryOnceAckJournal journal{};
  ArchiveBatchRuntime<MemoryImmutableBatchStore<8U>, RetryOnceAckJournal> runtime(
      &archive, &batches, &journal);
  StorageSink sink{};
  StorageClock clock{};
  runtime.bind_debug(&sink, InjectedMonotonicClock{read_storage_clock, &clock});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AppendStatus::Durable),
                        static_cast<int>(archive.append(sample(107U))));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchRuntimeStatus::Durable),
                        static_cast<int>(runtime.poll()));
  const Ack ack = ack_for(runtime.batch());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Retryable),
                        static_cast<int>(runtime.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(0U, archive.size());
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(runtime.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(3U, sink.count);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[2].arg0);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[2].arg1);
  TEST_ASSERT_EQUAL_UINT64(clock.now, sink.events[2].monotonic_us);
  TEST_ASSERT_EQUAL_UINT32(0U, archive.size());
  TEST_ASSERT_EQUAL_UINT32(0U, journal.inner.size());
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
  RUN_TEST(test_ack_recovery_reclaims_source_after_runtime_restart);
  RUN_TEST(test_ack_restart_uses_exact_persisted_batch_not_current_serializer);
  RUN_TEST(test_ack_persisted_manifest_validates_multirecord_count_and_reclaims_one_source);
  RUN_TEST(test_storage_ack_and_reclaim_events_follow_actual_durable_operations);
  RUN_TEST(test_refusing_storage_sink_cannot_change_ack_result_or_archive);
  RUN_TEST(test_ack_checkpoint_failure_emits_no_false_storage_completion);
  RUN_TEST(test_manifest_mismatch_emits_no_false_storage_completion);
  RUN_TEST(test_false_archive_reclaim_and_contains_variants_never_claim_reclaimed);
  RUN_TEST(test_archive_reclaim_is_reported_when_later_journal_reclaim_retries);
  return UNITY_END();
}
