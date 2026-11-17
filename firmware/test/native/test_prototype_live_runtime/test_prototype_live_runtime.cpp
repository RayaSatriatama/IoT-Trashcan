#include "acquisition/acquisition_types.h"
#include "prototype_live_runtime.h"
#include "storage/archive_store.h"
#include "storage/archive_codec.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace sigap {
namespace test {

// Synthetic host fixture for testing - NOT physical proof.
static void encode_synthetic_sample_into(
    const acquisition::DeviceId& device,
    const acquisition::BootId& boot,
    uint64_t sequence,
    acquisition::SensorId sensor,
    storage::ImmutableRecordBytes& out,
    int64_t counts_bias = 0) noexcept {
  acquisition::SampleRecord sample{};
  sample.header.device_id = device;
  sample.header.boot_id = boot;
  sample.header.sequence = sequence;
  sample.header.monotonic_us = sequence * 1000U;
  sample.header.sensor = sensor;
  sample.header.stream = acquisition::StreamId::Organic;
  sample.header.kind = acquisition::SampleKind::Hx711Counts;
  sample.quality = acquisition::SampleQuality::Uncalibrated;
  sample.reason = acquisition::SampleReason::ProfileMissing;
  sample.raw.present = true;
  sample.raw.signed_counts = static_cast<int64_t>(sequence * 100U) + counts_bias;
  out = storage::ImmutableRecordBytes{};
  (void)storage::RecordCodec::encode_sample(sample, out);
}

static storage::ImmutableRecordBytes make_synthetic_sample(
    const acquisition::DeviceId& device,
    const acquisition::BootId& boot,
    uint64_t sequence,
    acquisition::SensorId sensor) noexcept {
  storage::ImmutableRecordBytes encoded{};
  encode_synthetic_sample_into(device, boot, sequence, sensor, encoded);
  return encoded;
}

// Same identity, different payload: a source whose bytes changed after the
// batch was frozen. Distinct archive hash, same device/boot/sequence.
static storage::ImmutableRecordBytes make_changed_synthetic_sample(
    const acquisition::DeviceId& device,
    const acquisition::BootId& boot,
    uint64_t sequence,
    acquisition::SensorId sensor) noexcept {
  storage::ImmutableRecordBytes encoded{};
  encode_synthetic_sample_into(device, boot, sequence, sensor, encoded, 0x1000);
  return encoded;
}

// Synthetic boot-metadata record built with the released archive codec, so the
// non-sample one-record path is exercised through the real encoder/decoder.
static storage::ImmutableRecordBytes make_synthetic_boot_metadata(
    const acquisition::DeviceId& device,
    const acquisition::BootId& boot,
    uint64_t sequence) noexcept {
  acquisition::ConfigRef config{};
  config.present = true;
  config.schema_version = 1U;
  config.config_version = 1U;
  config.hash.fill(0xC3U);
  const storage::ArchiveIdentity identity{
      device, boot, sequence, storage::ArchiveRecordKind::Metadata};
  storage::ImmutableRecordBytes encoded{};
  storage::PayloadWriter writer{nullptr, 0U};
  if (!storage::RecordCodec::begin(identity, encoded, writer)) return encoded;
  constexpr char kFirmwareVersion[]{"test-fw"};
  if (!writer.u8(static_cast<uint8_t>(storage::ArchiveMetadataType::Boot)) ||
      !writer.u64(sequence * 1000U) || !writer.u8(0U) || !writer.i64(0) ||
      !writer.u8(static_cast<uint8_t>(acquisition::TimeQuality::Unsynced)) ||
      !writer.u32(config.schema_version) ||
      !writer.u64(config.config_version) ||
      !writer.u8(config.present ? 1U : 0U) ||
      !writer.bytes(config.hash.data(), config.hash.size()) ||
      !writer.u16(0U) ||
      !writer.u8(static_cast<uint8_t>(sizeof(kFirmwareVersion) - 1U)) ||
      !writer.bytes(reinterpret_cast<const uint8_t*>(kFirmwareVersion),
                    sizeof(kFirmwareVersion) - 1U) ||
      !writer.u8(1U) || !storage::RecordCodec::finish(encoded, writer)) {
    return storage::ImmutableRecordBytes{};
  }
  return encoded;
}

static acquisition::DeviceId make_device(uint8_t fill) noexcept {
  acquisition::DeviceId device{};
  device.present = true;
  device.bytes.fill(fill);
  return device;
}

static acquisition::BootId make_boot(uint8_t fill) noexcept {
  acquisition::BootId boot{};
  boot.present = true;
  boot.bytes.fill(fill);
  return boot;
}

static telemetry_archive::Ack ack_for(
    const telemetry_archive::ImmutableBatch& batch,
    const acquisition::DeviceId& device) noexcept {
  telemetry_archive::Ack ack{};
  ack.device_id = device;
  ack.batch_id_size = batch.batch_id_size;
  std::copy(batch.batch_id.begin(), batch.batch_id.begin() + batch.batch_id_size,
            ack.batch_id.begin());
  ack.body_hash = batch.body_hash;
  ack.record_count = batch.record_count;
  ack.stored_count = batch.record_count;
  ack.status = telemetry_archive::AckStatus::Durable;
  return ack;
}

static std::size_t count_occurrences(std::string_view haystack,
                                     std::string_view needle) noexcept {
  std::size_t count{0U};
  std::size_t position{0U};
  while ((position = haystack.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

// Monotonic test clock. Only this fixture advances it; the runtime must not
// read a real clock in host tests unless it is bound explicitly.
static uint64_t g_test_now_us{0U};
static uint64_t test_clock_reader(void*) noexcept { return g_test_now_us; }

bool test_prototype_batch_runtime_basic() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);
  runtime.bind_debug(nullptr, nullptr);

  const auto device = make_device(0xAA);
  const auto boot = make_boot(0xBB);
  const auto sample = make_synthetic_sample(device, boot, 1U,
                                            acquisition::SensorId::Hx711Organic);
  if (archive.append(sample) != storage::AppendStatus::Durable) {
    printf("[FAIL] archive append failed\n");
    return false;
  }

  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      !runtime.pending()) {
    printf("[FAIL] poll did not submit a pending batch\n");
    return false;
  }

  const auto& batch = runtime.batch();
  if (!batch.attempted || batch.record_count != 1U || batch.body_size == 0U) {
    printf("[FAIL] batch not properly built\n");
    return false;
  }

  auto forged_ack = ack_for(batch, device);
  forged_ack.device_id.present = false;
  const auto retained_id = batch.batch_id;
  const auto retained_hash = batch.body_hash;
  if (runtime.apply_ack(forged_ack) != telemetry_archive::AckPersistStatus::Invalid ||
      !runtime.pending() || archive.size() != 1U ||
      runtime.batch().batch_id != retained_id ||
      runtime.batch().body_hash != retained_hash) {
    printf("[FAIL] absent device ACK must preserve pending source/body\n");
    return false;
  }

  if (runtime.apply_ack(ack_for(runtime.batch(), device)) !=
          telemetry_archive::AckPersistStatus::Durable ||
      runtime.pending() || archive.size() != 0U) {
    printf("[FAIL] exact ACK did not reclaim the source\n");
    return false;
  }

  printf("[PASS] prototype batch runtime basic\n");
  return true;
}

bool test_prototype_batch_runtime_single_bytes_unchanged() {
  // A one-record group must serialize exactly like BatchJsonBuilder::build.
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0xA2);
  const auto boot = make_boot(0xB3);
  const auto sample = make_synthetic_sample(device, boot, 7U,
                                            acquisition::SensorId::Hx711Organic);
  if (archive.append(sample) != storage::AppendStatus::Durable ||
      runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable) {
    printf("[FAIL] single-record group poll failed\n");
    return false;
  }

  telemetry_archive::BatchJsonBuilder builder{};
  telemetry_archive::ImmutableBatch reference{};
  if (!builder.build(sample, reference)) {
    printf("[FAIL] reference build failed\n");
    return false;
  }
  const auto& batch = runtime.batch();
  const std::string_view actual =
      std::string_view(reinterpret_cast<const char*>(batch.body.data()),
                       batch.body_size);
  if (actual != reference.exact_body() || batch.record_count != 1U ||
      batch.body_hash != reference.body_hash ||
      batch.id() != reference.id()) {
    printf("[FAIL] single-record group bytes diverged from build(single)\n");
    return false;
  }

  printf("[PASS] single-record group bytes match build(single)\n");
  return true;
}

bool test_prototype_batch_runtime_multi_record_group() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0xC1);
  const auto boot = make_boot(0xD2);
  telemetry_archive::BatchJsonBuilder reference_builder{};
  std::array<std::string, 3U> expected_hashes{};
  for (uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    const auto record = make_synthetic_sample(
        device, boot, sequence, acquisition::SensorId::Hx711Organic);
    telemetry_archive::ImmutableBatch single{};
    if (!reference_builder.build(record, single)) {
      printf("[FAIL] reference single build %llu failed\n",
             static_cast<unsigned long long>(sequence));
      return false;
    }
    const std::string_view single_body = single.exact_body();
    const std::size_t hash_at = single_body.find("\"record_hash\":\"sha256:");
    if (hash_at == std::string_view::npos) {
      printf("[FAIL] reference body has no record_hash\n");
      return false;
    }
    expected_hashes[sequence - 1U] =
        std::string(single_body.substr(hash_at, 24U + 64U + 1U));
    if (archive.append(record) != storage::AppendStatus::Durable) {
      printf("[FAIL] append %llu failed\n",
             static_cast<unsigned long long>(sequence));
      return false;
    }
  }

  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable) {
    printf("[FAIL] multi-record poll did not build a batch\n");
    return false;
  }
  const auto& batch = runtime.batch();
  const std::string_view body =
      std::string_view(reinterpret_cast<const char*>(batch.body.data()),
                       batch.body_size);
  if (batch.record_count != 3U || runtime.manifest_size() != 3U) {
    printf("[FAIL] expected 3 grouped records, got %u\n",
           static_cast<unsigned>(batch.record_count));
    return false;
  }
  if (count_occurrences(body, "\"kind\":\"sample\"") != 3U ||
      count_occurrences(body, "\"record_hash\":\"sha256:") != 3U ||
      body.find("\"record_count\":3,\"samples\":[") == std::string_view::npos) {
    printf("[FAIL] grouped body shape is wrong\n");
    return false;
  }
  // Every per-record hash must be byte-identical to the single-record path.
  for (const auto& expected : expected_hashes) {
    if (body.find(expected) == std::string_view::npos) {
      printf("[FAIL] grouped body lost a per-record hash\n");
      return false;
    }
  }
  if (!batch.body_hash_matches()) {
    printf("[FAIL] grouped body hash does not match the frozen body\n");
    return false;
  }

  if (runtime.apply_ack(ack_for(batch, device)) !=
          telemetry_archive::AckPersistStatus::Durable ||
      runtime.pending() || archive.size() != 0U ||
      runtime.records_delivered() != 3U || runtime.batches_delivered() != 1U) {
    printf("[FAIL] exact whole-group ACK did not reclaim all sources\n");
    return false;
  }

  printf("[PASS] multi-record sample group build and reclaim\n");
  return true;
}

bool test_prototype_batch_runtime_group_stops_at_identity_change() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device_a = make_device(0x11);
  const auto device_b = make_device(0x22);
  const auto boot = make_boot(0x33);
  if (archive.append(make_synthetic_sample(device_a, boot, 1U,
                                           acquisition::SensorId::Hx711Organic)) !=
          storage::AppendStatus::Durable ||
      archive.append(make_synthetic_sample(device_b, boot, 2U,
                                           acquisition::SensorId::Hx711Organic)) !=
          storage::AppendStatus::Durable) {
    printf("[FAIL] append failed\n");
    return false;
  }

  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 1U || archive.size() != 2U) {
    printf("[FAIL] group must stop before a different device identity\n");
    return false;
  }
  if (runtime.apply_ack(ack_for(runtime.batch(), device_a)) !=
          telemetry_archive::AckPersistStatus::Durable ||
      archive.size() != 1U) {
    printf("[FAIL] first group did not reclaim exactly one source\n");
    return false;
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 1U ||
      runtime.batch().device_id.bytes != device_b.bytes) {
    printf("[FAIL] second device was not batchable after the first\n");
    return false;
  }

  printf("[PASS] group stops at identity change without dropping records\n");
  return true;
}

bool test_prototype_batch_runtime_cycles_beyond_capacity() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0x44);
  const auto boot = make_boot(0x55);
  uint64_t sequence{1U};
  uint64_t delivered{0U};
  // 40 records through a 16-slot store: three full groups plus a remainder.
  for (int cycle = 0; cycle < 4; ++cycle) {
    for (int index = 0; index < 10; ++index) {
      if (archive.append(make_synthetic_sample(
              device, boot, sequence++, acquisition::SensorId::Hx711Organic)) !=
          storage::AppendStatus::Durable) {
        printf("[FAIL] cycle %d append %d failed\n", cycle, index);
        return false;
      }
    }
    while (runtime.poll() == telemetry_archive::BatchRuntimeStatus::Durable) {
      delivered += runtime.batch().record_count;
      if (runtime.apply_ack(ack_for(runtime.batch(), device)) !=
          telemetry_archive::AckPersistStatus::Durable) {
        printf("[FAIL] cycle %d ACK failed\n", cycle);
        return false;
      }
    }
  }
  if (delivered != 40U || archive.size() != 0U ||
      runtime.records_delivered() != 40U) {
    printf("[FAIL] expected 40 delivered records, got %llu\n",
           static_cast<unsigned long long>(delivered));
    return false;
  }

  printf("[PASS] multi-record cycling beyond RAM capacity loses no record\n");
  return true;
}

bool test_prototype_batch_runtime_wrong_id() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0xEE);
  const auto boot = make_boot(0xFF);
  archive.append(make_synthetic_sample(device, boot, 1U,
                                       acquisition::SensorId::Hx711Organic));
  runtime.poll();

  auto ack = ack_for(runtime.batch(), device);
  for (std::size_t index = 0; index < ack.batch_id_size; ++index) {
    ack.batch_id[index] = static_cast<char>(ack.batch_id[index] ^ 0xFF);
  }
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Invalid ||
      !runtime.pending()) {
    printf("[FAIL] wrong batch ID must be Invalid and stay pending\n");
    return false;
  }

  printf("[PASS] prototype batch runtime wrong ID rejection\n");
  return true;
}

bool test_prototype_batch_runtime_wrong_body_hash() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0x12);
  const auto boot = make_boot(0x23);
  archive.append(make_synthetic_sample(device, boot, 1U,
                                       acquisition::SensorId::Hx711Organic));
  runtime.poll();

  auto ack = ack_for(runtime.batch(), device);
  ack.body_hash[0] = static_cast<uint8_t>(ack.body_hash[0] ^ 0xFFU);
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Invalid ||
      !runtime.pending() || archive.size() != 1U) {
    printf("[FAIL] wrong body hash must be Invalid and not reclaim\n");
    return false;
  }

  printf("[PASS] prototype batch runtime wrong body hash rejection\n");
  return true;
}

bool test_prototype_batch_runtime_partial_count_ack() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0x34);
  const auto boot = make_boot(0x45);
  for (uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    archive.append(make_synthetic_sample(device, boot, sequence,
                                         acquisition::SensorId::Hx711Organic));
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 3U) {
    printf("[FAIL] group was not built\n");
    return false;
  }

  // Wrong declared count.
  auto wrong_count = ack_for(runtime.batch(), device);
  wrong_count.record_count = 2U;
  wrong_count.stored_count = 2U;
  if (runtime.apply_ack(wrong_count) != telemetry_archive::AckPersistStatus::Invalid ||
      !runtime.pending() || archive.size() != 3U) {
    printf("[FAIL] wrong record count must be Invalid and not reclaim\n");
    return false;
  }

  // Correct count but a partial stored+deduplicated split.
  auto partial = ack_for(runtime.batch(), device);
  partial.stored_count = 2U;
  partial.deduplicated_count = 0U;
  if (runtime.apply_ack(partial) != telemetry_archive::AckPersistStatus::Invalid ||
      !runtime.pending() || archive.size() != 3U) {
    printf("[FAIL] stored+deduplicated must equal the frozen count\n");
    return false;
  }

  printf("[PASS] prototype batch runtime rejects partial/wrong count ACKs\n");
  return true;
}

bool test_prototype_batch_runtime_pending_body_frozen() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0x56);
  const auto boot = make_boot(0x67);
  for (uint64_t sequence = 1U; sequence <= 2U; ++sequence) {
    archive.append(make_synthetic_sample(device, boot, sequence,
                                         acquisition::SensorId::Hx711Organic));
  }
  runtime.poll();
  const auto body_hash = runtime.batch().body_hash;
  const auto batch_id = runtime.batch().batch_id;
  const auto body_size = runtime.batch().body_size;

  // New arrivals and repeated polls must not rebuild the in-flight body.
  archive.append(make_synthetic_sample(device, boot, 3U,
                                       acquisition::SensorId::Hx711Organic));
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable) {
    printf("[FAIL] pending poll must report Durable\n");
    return false;
  }
  if (runtime.batch().body_hash != body_hash ||
      runtime.batch().batch_id != batch_id ||
      runtime.batch().body_size != body_size ||
      runtime.batch().record_count != 2U) {
    printf("[FAIL] in-flight body changed while pending\n");
    return false;
  }

  printf("[PASS] in-flight body and manifest stay frozen\n");
  return true;
}

// Store whose reclaim can be made to fail for one call, to exercise the
// partial-reclaim retry path without touching production stores.
class FlakyReclaimArchive final : public storage::IArchiveStore {
 public:
  storage::MemoryArchiveStore<16U> inner{64U * 1024U};
  int fail_reclaim_call{-1};
  int reclaim_calls{0};

  storage::AppendStatus append(const storage::ImmutableRecordBytes& value) noexcept override {
    return inner.append(value);
  }
  storage::AppendStatus append_gap(const storage::GapRange& value) noexcept override {
    return inner.append_gap(value);
  }
  bool checkpoint() noexcept override { return inner.checkpoint(); }
  bool recover() noexcept override { return inner.recover(); }
  uint64_t pending_bytes() const noexcept override { return inner.pending_bytes(); }
  uint64_t pending_quota_bytes() const noexcept override {
    return inner.pending_quota_bytes();
  }
  storage::ArchiveHealth health() const noexcept override { return inner.health(); }
  bool peek_pending(storage::ImmutableRecordBytes& value) noexcept override {
    return inner.peek_pending(value);
  }
  bool peek_pending_at(std::size_t index,
                       storage::ImmutableRecordBytes& value) noexcept override {
    return inner.peek_pending_at(index, value);
  }
  bool peek_pending_identity_at(std::size_t index,
                                storage::ArchiveIdentity& value) noexcept override {
    return inner.peek_pending_identity_at(index, value);
  }
  bool contains(const storage::ArchiveIdentity& identity,
                const config::ConfigHash& hash) noexcept override {
    return inner.contains(identity, hash);
  }
  bool reclaim_pending(const storage::ArchiveIdentity& identity,
                       const config::ConfigHash& hash) noexcept override {
    if (fail_reclaim_call >= 0 && reclaim_calls == fail_reclaim_call) {
      ++reclaim_calls;
      return false;
    }
    ++reclaim_calls;
    return inner.reclaim_pending(identity, hash);
  }
};

bool test_prototype_batch_runtime_partial_reclaim_retry() {
  FlakyReclaimArchive archive;
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0x78);
  const auto boot = make_boot(0x89);
  for (uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    archive.append(make_synthetic_sample(device, boot, sequence,
                                         acquisition::SensorId::Hx711Organic));
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 3U) {
    printf("[FAIL] group was not built\n");
    return false;
  }

  const auto ack = ack_for(runtime.batch(), device);
  archive.fail_reclaim_call = 1;  // first reclaim succeeds, second fails
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Retryable ||
      !runtime.pending() || runtime.reclaimed_size() != 1U ||
      archive.inner.size() != 2U || runtime.partial_reclaim_count() != 1U ||
      runtime.source_mismatch_count() != 0U) {
    printf("[FAIL] partial reclaim must be Retryable and keep progress\n");
    return false;
  }

  // Two more failing retries of the same frozen ACK: the per-batch counter must
  // stay at 1 and the retained prefix must not be charged again.
  archive.fail_reclaim_call = 2;
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Retryable ||
      !runtime.pending() || runtime.reclaimed_size() != 1U ||
      archive.inner.size() != 2U || runtime.partial_reclaim_count() != 1U) {
    printf("[FAIL] repeated partial failure must not double the counter\n");
    return false;
  }
  archive.fail_reclaim_call = 3;
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Retryable ||
      !runtime.pending() || runtime.reclaimed_size() != 1U ||
      runtime.partial_reclaim_count() != 1U) {
    printf("[FAIL] third partial failure must keep the counter at 1\n");
    return false;
  }

  // Retry with the same frozen ACK: the retired prefix must not be required
  // again and the remaining two sources must be reclaimed exactly once.
  archive.fail_reclaim_call = -1;
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Durable ||
      runtime.pending() || archive.inner.size() != 0U ||
      runtime.records_delivered() != 3U ||
      runtime.partial_reclaim_count() != 1U) {
    printf("[FAIL] reclaim retry did not resume from retained progress\n");
    return false;
  }

  printf("[PASS] partial reclaim retry is idempotent and head-first\n");
  return true;
}

bool test_prototype_batch_runtime_source_lost_before_ack() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0x9A);
  const auto boot = make_boot(0xAB);
  for (uint64_t sequence = 1U; sequence <= 2U; ++sequence) {
    archive.append(make_synthetic_sample(device, boot, sequence,
                                         acquisition::SensorId::Hx711Organic));
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 2U) {
    printf("[FAIL] group was not built\n");
    return false;
  }
  const auto frozen_id = runtime.batch().batch_id;
  const auto frozen_hash = runtime.batch().body_hash;
  const auto frozen_size = runtime.batch().body_size;
  const std::string frozen_body(
      reinterpret_cast<const char*>(runtime.batch().body.data()),
      runtime.batch().body_size);

  // Simulate an external consumer that already retired the FIFO head before
  // the ACK arrived. The frozen manifest can no longer be honoured.
  storage::ImmutableRecordBytes head{};
  if (!archive.peek_pending(head) ||
      !archive.reclaim_pending(head.identity, head.hash) ||
      archive.size() != 1U) {
    printf("[FAIL] fixture could not remove the head\n");
    return false;
  }

  // A vanished source must be non-Durable but must NOT abandon the frozen
  // batch: body, ID, hash, count, manifest and reclaim cursor stay intact and
  // nothing further is reclaimed.
  const auto status = runtime.apply_ack(ack_for(runtime.batch(), device));
  if (status == telemetry_archive::AckPersistStatus::Durable ||
      status != telemetry_archive::AckPersistStatus::Retryable ||
      !runtime.pending() || archive.size() != 1U ||
      runtime.source_mismatch_count() != 1U ||
      runtime.records_delivered() != 0U ||
      runtime.batches_delivered() != 0U ||
      runtime.manifest_size() != 2U || runtime.reclaimed_size() != 0U) {
    printf("[FAIL] lost source must retain the frozen batch and cursor\n");
    return false;
  }
  const std::string retained_body(
      reinterpret_cast<const char*>(runtime.batch().body.data()),
      runtime.batch().body_size);
  if (runtime.batch().batch_id != frozen_id ||
      runtime.batch().body_hash != frozen_hash ||
      runtime.batch().body_size != frozen_size ||
      runtime.batch().record_count != 2U || retained_body != frozen_body) {
    printf("[FAIL] lost source changed the frozen body or manifest\n");
    return false;
  }

  // A repeated retry of the same fault must not double the per-batch counter,
  // and it must keep returning non-Durable with the batch retained.
  for (int retry = 0; retry < 2; ++retry) {
    if (runtime.apply_ack(ack_for(runtime.batch(), device)) !=
            telemetry_archive::AckPersistStatus::Retryable ||
        !runtime.pending() || runtime.source_mismatch_count() != 1U ||
        runtime.records_delivered() != 0U || archive.size() != 1U) {
      printf("[FAIL] repeated lost-source retry must keep one batch event\n");
      return false;
    }
  }

  // A repeated poll must report the same pending batch, never rebuild a
  // different body under the same first-derived batch ID.
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      !runtime.pending() || runtime.manifest_size() != 2U ||
      runtime.batch().batch_id != frozen_id ||
      runtime.batch().body_hash != frozen_hash ||
      runtime.batch().record_count != 2U || archive.size() != 1U) {
    printf("[FAIL] poll rebuilt a batch behind a retained frozen manifest\n");
    return false;
  }

  printf("[PASS] lost source retains the frozen batch, cursor and body\n");
  return true;
}

bool test_prototype_batch_runtime_source_changed_before_ack() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0xA3);
  const auto boot = make_boot(0xB4);
  for (uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    archive.append(make_synthetic_sample(device, boot, sequence,
                                         acquisition::SensorId::Hx711Organic));
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 3U || archive.size() != 3U) {
    printf("[FAIL] group was not built\n");
    return false;
  }
  const auto frozen_id = runtime.batch().batch_id;
  const auto frozen_hash = runtime.batch().body_hash;
  const auto frozen_size = runtime.batch().body_size;
  const std::string frozen_body(
      reinterpret_cast<const char*>(runtime.batch().body.data()),
      runtime.batch().body_size);
  const auto ack = ack_for(runtime.batch(), device);

  // Replace the middle archived source with a different record of the SAME
  // identity, while the first record still matches. This is done through the
  // store's own append/reclaim seam (an external consumer of the same archive),
  // not by forging the ACK or writing through the runtime.
  storage::ImmutableRecordBytes first_copy{};
  storage::ImmutableRecordBytes middle_copy{};
  storage::ImmutableRecordBytes last_copy{};
  if (!archive.peek_pending_at(0U, first_copy) ||
      !archive.peek_pending_at(1U, middle_copy) ||
      !archive.peek_pending_at(2U, last_copy) ||
      !archive.reclaim_pending(first_copy.identity, first_copy.hash) ||
      !archive.reclaim_pending(middle_copy.identity, middle_copy.hash) ||
      !archive.reclaim_pending(last_copy.identity, last_copy.hash) ||
      archive.size() != 0U ||
      archive.append(first_copy) != storage::AppendStatus::Durable ||
      archive.append(make_changed_synthetic_sample(
          device, boot, 2U, acquisition::SensorId::Hx711Organic)) !=
          storage::AppendStatus::Durable ||
      archive.append(last_copy) != storage::AppendStatus::Durable ||
      archive.size() != 3U ||
      archive.at(1U).hash == middle_copy.hash ||
      archive.at(0U).hash != first_copy.hash) {
    printf("[FAIL] fixture could not change the middle source\n");
    return false;
  }

  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Retryable ||
      !runtime.pending() || archive.size() != 3U ||
      runtime.reclaimed_size() != 0U ||
      runtime.source_mismatch_count() != 1U ||
      runtime.records_delivered() != 0U ||
      runtime.batches_delivered() != 0U) {
    printf("[FAIL] changed middle source must not reclaim or deliver\n");
    return false;
  }
  const std::string retained_body(
      reinterpret_cast<const char*>(runtime.batch().body.data()),
      runtime.batch().body_size);
  if (runtime.batch().batch_id != frozen_id ||
      runtime.batch().body_hash != frozen_hash ||
      runtime.batch().body_size != frozen_size ||
      runtime.batch().record_count != 3U || runtime.manifest_size() != 3U ||
      retained_body != frozen_body) {
    printf("[FAIL] changed source rebuilt or cleared the frozen batch\n");
    return false;
  }

  // Restoring the exact source lets the SAME frozen ACK complete the original
  // group, so the retained body still describes a deliverable batch.
  if (!archive.reclaim_pending(first_copy.identity, first_copy.hash) ||
      !archive.reclaim_pending(archive.at(0U).identity, archive.at(0U).hash) ||
      !archive.reclaim_pending(last_copy.identity, last_copy.hash) ||
      archive.size() != 0U ||
      archive.append(first_copy) != storage::AppendStatus::Durable ||
      archive.append(middle_copy) != storage::AppendStatus::Durable ||
      archive.append(last_copy) != storage::AppendStatus::Durable ||
      archive.size() != 3U ||
      archive.at(1U).hash != middle_copy.hash) {
    printf("[FAIL] fixture could not restore the original sources\n");
    return false;
  }
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Durable ||
      runtime.pending() || archive.size() != 0U ||
      runtime.records_delivered() != 3U ||
      runtime.batches_delivered() != 1U) {
    printf("[FAIL] restored source did not complete the original group\n");
    return false;
  }

  printf("[PASS] changed middle source retains the batch until restored\n");
  return true;
}

bool test_prototype_batch_runtime_source_fault_after_partial_reclaim() {
  FlakyReclaimArchive archive;
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0xB5);
  const auto boot = make_boot(0xC6);
  for (uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    archive.append(make_synthetic_sample(device, boot, sequence,
                                         acquisition::SensorId::Hx711Organic));
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 3U) {
    printf("[FAIL] group was not built\n");
    return false;
  }
  const auto frozen_id = runtime.batch().batch_id;
  const auto frozen_hash = runtime.batch().body_hash;
  const auto frozen_size = runtime.batch().body_size;
  const std::string frozen_body(
      reinterpret_cast<const char*>(runtime.batch().body.data()),
      runtime.batch().body_size);
  const auto ack = ack_for(runtime.batch(), device);

  // First reclaim succeeds, second fails: the completed prefix and cursor are
  // retained for the retry.
  archive.fail_reclaim_call = 1;
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Retryable ||
      !runtime.pending() || runtime.reclaimed_size() != 1U ||
      archive.inner.size() != 2U || archive.reclaim_calls != 2) {
    printf("[FAIL] partial reclaim must retain the completed prefix\n");
    return false;
  }

  // Now the next remaining source changes. The fault must be reported
  // non-Durable with the cursor and body retained and no further reclaim.
  // Injection is a test-local edit of the non-const fixture store; it does not
  // call the store's reclaim seam, so reclaim_calls still counts only the
  // runtime's own attempts.
  const auto remaining_head_hash = archive.inner.at(0U).hash;
  storage::ImmutableRecordBytes changed_copy = make_changed_synthetic_sample(
      device, boot, 2U, acquisition::SensorId::Hx711Organic);
  if (changed_copy.size == 0U || changed_copy.hash == remaining_head_hash) {
    printf("[FAIL] changed-source fixture is not distinct\n");
    return false;
  }
  const_cast<storage::ImmutableRecordBytes&>(archive.inner.at(0U)) = changed_copy;
  if (archive.inner.at(0U).hash != changed_copy.hash ||
      archive.inner.size() != 2U || archive.reclaim_calls != 2) {
    printf("[FAIL] fixture could not change the next remaining source\n");
    return false;
  }
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Retryable ||
      !runtime.pending() || runtime.reclaimed_size() != 1U ||
      archive.inner.size() != 2U || archive.reclaim_calls != 2 ||
      runtime.source_mismatch_count() != 1U ||
      runtime.records_delivered() != 0U) {
    printf("[FAIL] post-prefix source fault must retain cursor and body\n");
    return false;
  }
  const std::string retained_body(
      reinterpret_cast<const char*>(runtime.batch().body.data()),
      runtime.batch().body_size);
  if (runtime.batch().batch_id != frozen_id ||
      runtime.batch().body_hash != frozen_hash ||
      runtime.batch().body_size != frozen_size ||
      runtime.batch().record_count != 3U || retained_body != frozen_body) {
    printf("[FAIL] post-prefix source fault changed the frozen batch\n");
    return false;
  }

  // Restoring the source completes with the same ACK, and the already
  // reclaimed prefix is never required again: the cursor resumes at record 2.
  const auto restored = make_synthetic_sample(device, boot, 2U,
                                              acquisition::SensorId::Hx711Organic);
  if (restored.hash != remaining_head_hash) {
    printf("[FAIL] restore fixture is not the original source\n");
    return false;
  }
  const_cast<storage::ImmutableRecordBytes&>(archive.inner.at(0U)) = restored;
  if (archive.inner.at(0U).hash != remaining_head_hash ||
      archive.reclaim_calls != 2) {
    printf("[FAIL] fixture could not restore the remaining source\n");
    return false;
  }
  if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Durable ||
      runtime.pending() || archive.inner.size() != 0U ||
      runtime.records_delivered() != 3U ||
      runtime.batches_delivered() != 1U || archive.reclaim_calls != 4 ||
      runtime.partial_reclaim_count() != 1U) {
    printf("[FAIL] retained prefix was reclaimed again or delivery failed\n");
    return false;
  }

  printf("[PASS] source fault after a reclaimed prefix keeps the cursor\n");
  return true;
}

bool test_prototype_batch_runtime_failed_reclaim_preserves_pending() {
  class FailedReclaimArchive final : public storage::IArchiveStore {
   public:
    storage::MemoryArchiveStore<16U> inner{64U * 1024U};
    storage::AppendStatus append(const storage::ImmutableRecordBytes& value) noexcept override { return inner.append(value); }
    storage::AppendStatus append_gap(const storage::GapRange& value) noexcept override { return inner.append_gap(value); }
    bool checkpoint() noexcept override { return inner.checkpoint(); }
    bool recover() noexcept override { return inner.recover(); }
    uint64_t pending_bytes() const noexcept override { return inner.pending_bytes(); }
    uint64_t pending_quota_bytes() const noexcept override { return inner.pending_quota_bytes(); }
    storage::ArchiveHealth health() const noexcept override { return inner.health(); }
    bool peek_pending(storage::ImmutableRecordBytes& value) noexcept override { return inner.peek_pending(value); }
    bool peek_pending_at(std::size_t index,
                         storage::ImmutableRecordBytes& value) noexcept override { return inner.peek_pending_at(index, value); }
    bool peek_pending_identity_at(std::size_t index,
                                  storage::ArchiveIdentity& value) noexcept override { return inner.peek_pending_identity_at(index, value); }
    bool contains(const storage::ArchiveIdentity& id,
                  const config::ConfigHash& hash) noexcept override { return inner.contains(id, hash); }
    bool reclaim_pending(const storage::ArchiveIdentity&,
                         const config::ConfigHash&) noexcept override { return false; }
  };

  FailedReclaimArchive archive;
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);
  const auto device = make_device(0x91);
  const auto boot = make_boot(0x92);
  if (archive.append(make_synthetic_sample(device, boot, 1U,
          acquisition::SensorId::Hx711Organic)) != storage::AppendStatus::Durable ||
      runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable) {
    return false;
  }

  const auto original_id = runtime.batch().batch_id;
  const auto original_hash = runtime.batch().body_hash;
  const auto ack = ack_for(runtime.batch(), device);
  // A total reclaim failure makes no progress, so it is not a partial reclaim:
  // the per-batch partial counter must stay 0 across repeated retries.
  for (int retry = 0; retry < 3; ++retry) {
    if (runtime.apply_ack(ack) != telemetry_archive::AckPersistStatus::Retryable ||
        !runtime.pending() || archive.inner.size() != 1U ||
        runtime.reclaimed_size() != 0U ||
        runtime.partial_reclaim_count() != 0U ||
        runtime.source_mismatch_count() != 0U ||
        runtime.batch().batch_id != original_id ||
        runtime.batch().body_hash != original_hash) {
      printf("[FAIL] failed reclaim must preserve the frozen batch\n");
      return false;
    }
  }
  printf("[PASS] failed reclaim preserves pending batch\n");
  return true;
}

bool test_prototype_batch_runtime_byte_bound() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0xCD);
  const auto boot = make_boot(0xDE);
  for (uint64_t sequence = 1U; sequence <= PrototypeLiveBatchRuntime::kMaxGroupRecords;
       ++sequence) {
    if (archive.append(make_synthetic_sample(device, boot, sequence,
                                             acquisition::SensorId::Hx711Organic)) !=
        storage::AppendStatus::Durable) {
      printf("[FAIL] append %llu failed\n", static_cast<unsigned long long>(sequence));
      return false;
    }
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable) {
    printf("[FAIL] bounded group was not built\n");
    return false;
  }
  const auto& batch = runtime.batch();
  if (batch.body_size > telemetry_archive::kMaxBatchBytes ||
      batch.record_count > telemetry_archive::kMaxBatchRecords ||
      batch.record_count != PrototypeLiveBatchRuntime::kMaxGroupRecords ||
      !batch.body_hash_matches()) {
    printf("[FAIL] grouped body violates the frozen byte/count bound\n");
    return false;
  }

  printf("[PASS] grouped body respects 32 KiB / 128-record bounds\n");
  return true;
}

bool test_prototype_batch_runtime_metadata_batch_counted() {
  // The non-sample (metadata/event/result) path must count a built batch too,
  // otherwise batches_delivered_ can exceed batches_built_ after boot metadata.
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);

  const auto device = make_device(0x5C);
  const auto boot = make_boot(0x6D);
  const auto metadata = make_synthetic_boot_metadata(device, boot, 1U);
  if (metadata.size == 0U ||
      archive.append(metadata) != storage::AppendStatus::Durable) {
    printf("[FAIL] metadata fixture could not be archived\n");
    return false;
  }

  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      !runtime.pending() || runtime.batch().record_count != 1U ||
      runtime.batches_built() != 1U || runtime.batches_delivered() != 0U) {
    printf("[FAIL] metadata batch was not counted as built\n");
    return false;
  }
  const std::string_view body =
      std::string_view(reinterpret_cast<const char*>(runtime.batch().body.data()),
                       runtime.batch().body_size);
  if (body.find("\"kind\":\"metadata\"") == std::string_view::npos ||
      body.find("\"record_count\":1") == std::string_view::npos) {
    printf("[FAIL] metadata batch body shape is wrong\n");
    return false;
  }
  if (runtime.apply_ack(ack_for(runtime.batch(), device)) !=
          telemetry_archive::AckPersistStatus::Durable ||
      runtime.pending() || archive.size() != 0U ||
      runtime.batches_built() != 1U || runtime.batches_delivered() != 1U ||
      runtime.records_delivered() != 1U) {
    printf("[FAIL] metadata batch delivered count diverged from built\n");
    return false;
  }

  // A following sample batch keeps built and delivered comparable.
  if (archive.append(make_synthetic_sample(device, boot, 2U,
                                           acquisition::SensorId::Hx711Organic)) !=
          storage::AppendStatus::Durable ||
      runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batches_built() != 2U) {
    printf("[FAIL] sample batch after metadata was not counted\n");
    return false;
  }
  if (runtime.apply_ack(ack_for(runtime.batch(), device)) !=
          telemetry_archive::AckPersistStatus::Durable ||
      runtime.batches_built() != 2U || runtime.batches_delivered() != 2U ||
      runtime.records_delivered() != 2U ||
      runtime.batches_built() < runtime.batches_delivered()) {
    printf("[FAIL] built/delivered counters are not comparable\n");
    return false;
  }

  printf("[PASS] metadata and sample batches both count as built\n");
  return true;
}

bool test_sample_group_builder_byte_bound_direct() {
  // Direct builder oracle for the 32 KiB byte bound: max_records is the full
  // 128-record contract bound, so the group must stop on bytes (Full) before
  // the count cap. One scratch record is reused for every add; no array of
  // record-sized buffers is created.
  const auto device = make_device(0xC7);
  const auto boot = make_boot(0xD8);
  const auto first = make_synthetic_sample(device, boot, 1U,
                                           acquisition::SensorId::Hx711Organic);
  if (first.size == 0U) {
    printf("[FAIL] first fixture record was not encoded\n");
    return false;
  }

  // Small begin() rejections: the frozen contract is 1..128 records.
  {
    telemetry_archive::ImmutableBatch rejected{};
    telemetry_archive::BatchJsonBuilder::SampleGroupBuilder group{};
    if (group.begin(rejected, first.identity, 0U) ||
        group.begin(rejected, first.identity,
                    static_cast<uint16_t>(telemetry_archive::kMaxBatchRecords + 1U))) {
      printf("[FAIL] max_records outside 1..128 must be rejected\n");
      return false;
    }
  }

  telemetry_archive::ImmutableBatch batch{};
  telemetry_archive::BatchJsonBuilder::SampleGroupBuilder group{};
  if (!group.begin(batch, first.identity,
                   telemetry_archive::kMaxBatchRecords)) {
    printf("[FAIL] 128-record group did not begin\n");
    return false;
  }

  storage::ImmutableRecordBytes scratch{};
  uint64_t sequence{1U};
  bool reached_full{false};
  while (sequence <= telemetry_archive::kMaxBatchRecords) {
    encode_synthetic_sample_into(device, boot, sequence,
                                 acquisition::SensorId::Hx711Organic, scratch);
    if (scratch.size == 0U) {
      printf("[FAIL] fixture record %llu was not encoded\n",
             static_cast<unsigned long long>(sequence));
      return false;
    }
    const auto status = group.add(scratch);
    if (status == telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::
                      AddStatus::Full) {
      reached_full = true;
      break;
    }
    if (status != telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::
                      AddStatus::Added) {
      printf("[FAIL] valid unique record %llu was not added\n",
             static_cast<unsigned long long>(sequence));
      return false;
    }
    ++sequence;
  }
  if (!reached_full) {
    printf("[FAIL] byte bound was not reached before the count cap\n");
    return false;
  }
  // The Full record must not have been counted, and the bound must be bytes,
  // not the 128-record cap. Boundary actually reached here: a record whose
  // serialized form does not fit the remaining record area (write_sample stops
  // partway -> Full). The exact-fit kSuffixReserve arithmetic case is covered
  // by the arithmetic adjudication, not by this fixture.
  const std::size_t accepted = group.count();
  if (accepted == 0U || accepted >= telemetry_archive::kMaxBatchRecords) {
    printf("[FAIL] group stopped on the count cap, not the byte bound\n");
    return false;
  }
  if (group.add(scratch) !=
          telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::AddStatus::Full ||
      group.count() != accepted) {
    printf("[FAIL] a rejected Full add changed the accepted count\n");
    return false;
  }
  if (!group.finish(batch)) {
    printf("[FAIL] byte-bound group did not finish\n");
    return false;
  }
  if (!batch.body_hash_matches() ||
      batch.body_size > telemetry_archive::kMaxBatchBytes ||
      batch.record_count != static_cast<uint16_t>(accepted) ||
      batch.exact_body().find("\"record_count\":") == std::string_view::npos) {
    printf("[FAIL] finished body violates the byte/count/hash bound\n");
    return false;
  }

  // The rejected source is untouched and starts the next group.
  if (scratch.size == 0U || !storage::RecordCodec::valid(scratch)) {
    printf("[FAIL] rejected source was modified by the Full add\n");
    return false;
  }
  telemetry_archive::ImmutableBatch next{};
  telemetry_archive::BatchJsonBuilder::SampleGroupBuilder next_group{};
  if (!next_group.begin(next, scratch.identity,
                        telemetry_archive::kMaxBatchRecords) ||
      next_group.add(scratch) !=
          telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::
              AddStatus::Added ||
      !next_group.finish(next) || !next.body_hash_matches()) {
    printf("[FAIL] rejected source could not start the next group\n");
    return false;
  }

  // A one-record cap reports Full on the second record without changing count.
  telemetry_archive::ImmutableBatch single{};
  telemetry_archive::BatchJsonBuilder::SampleGroupBuilder single_group{};
  if (!single_group.begin(single, first.identity, 1U)) {
    printf("[FAIL] one-record group did not begin\n");
    return false;
  }
  const auto second = make_synthetic_sample(device, boot, 2U,
                                            acquisition::SensorId::Hx711Organic);
  if (single_group.add(first) !=
          telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::
              AddStatus::Added ||
      single_group.add(second) !=
          telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::
              AddStatus::Full ||
      single_group.count() != 1U) {
    printf("[FAIL] one-record cap did not report Full\n");
    return false;
  }

  printf("[PASS] direct builder stops on the byte bound before the count cap\n");
  return true;
}

bool test_prototype_batch_runtime_bounded_accumulation() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);
  runtime.bind_clock(acquisition::InjectedMonotonicClock{test_clock_reader, nullptr});

  const auto device = make_device(0xE1);
  const auto boot = make_boot(0xF2);
  for (uint64_t sequence = 1U; sequence <= 2U; ++sequence) {
    archive.append(make_synthetic_sample(device, boot, sequence,
                                         acquisition::SensorId::Hx711Organic));
  }

  g_test_now_us = 1000U;
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Idle ||
      runtime.pending()) {
    printf("[FAIL] bounded accumulation must not build below the target\n");
    return false;
  }

  g_test_now_us = 1000U + PrototypeLiveBatchRuntime::kBatchMaxWaitUs;
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != 2U) {
    printf("[FAIL] accumulation window did not flush the available records\n");
    return false;
  }

  printf("[PASS] bounded accumulation defers then flushes a small group\n");
  return true;
}

// Source oracle only until the owner releases native execution.
bool test_prototype_batch_runtime_unordered_unique_records() {
  storage::MemoryArchiveStore<16> archive(64 * 1024);
  PrototypeLiveBatchRuntime runtime;
  runtime.bind(&archive);
  const auto device = make_device(0xA1);
  const auto boot = make_boot(0xB2);
  const std::array<uint64_t, 8> sequences{{40U, 12U, 33U, 11U, 39U, 21U, 31U, 10U}};
  for (const auto sequence : sequences) {
    if (archive.append(make_synthetic_sample(device, boot, sequence,
          acquisition::SensorId::Hx711Organic)) != storage::AppendStatus::Durable) return false;
  }
  if (runtime.poll() != telemetry_archive::BatchRuntimeStatus::Durable ||
      runtime.batch().record_count != sequences.size()) return false;
  const auto& batch = runtime.batch();
  const std::string_view body{reinterpret_cast<const char*>(batch.body.data()), batch.body_size};
  std::size_t position{0U};
  for (const auto sequence : sequences) {
    const std::string needle = "\"sequence\":\"" + std::to_string(sequence) + "\"";
    const auto found = body.find(needle, position);
    if (found == std::string_view::npos) return false;
    position = found + needle.size();
  }
  if (runtime.apply_ack(ack_for(batch, device)) != telemetry_archive::AckPersistStatus::Durable ||
      runtime.pending() || runtime.records_delivered() != sequences.size()) return false;
  printf("[PASS] unordered unique sequences retain FIFO serialization and whole-batch ACK\n");
  return true;
}

bool test_sample_group_builder_rejects_nonadjacent_duplicate() {
  telemetry_archive::ImmutableBatch batch;
  telemetry_archive::BatchJsonBuilder::SampleGroupBuilder group;
  const auto device = make_device(0xA1);
  const auto boot = make_boot(0xB2);
  const auto first = make_synthetic_sample(device, boot, 40U, acquisition::SensorId::Hx711Organic);
  const auto lower = make_synthetic_sample(device, boot, 12U, acquisition::SensorId::Hx711Organic);
  const auto changed = make_changed_synthetic_sample(device, boot, 40U, acquisition::SensorId::Hx711Organic);
  using Status = telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::AddStatus;
  if (!group.begin(batch, first.identity, 8U) ||
      group.add(first) != Status::Added || group.add(lower) != Status::Added ||
      group.add(first) != Status::Invalid || group.add(changed) != Status::Invalid ||
      group.count() != 2U || !group.finish(batch) || batch.record_count != 2U) return false;
  printf("[PASS] duplicate identity is rejected even after a lower sequence and changed payload\n");
  return true;
}

}  // namespace test
}  // namespace sigap

int main() {
  bool all_pass = true;
  all_pass &= sigap::test::test_prototype_batch_runtime_basic();
  all_pass &= sigap::test::test_prototype_batch_runtime_single_bytes_unchanged();
  all_pass &= sigap::test::test_prototype_batch_runtime_multi_record_group();
  all_pass &= sigap::test::test_prototype_batch_runtime_group_stops_at_identity_change();
  all_pass &= sigap::test::test_prototype_batch_runtime_cycles_beyond_capacity();
  all_pass &= sigap::test::test_prototype_batch_runtime_wrong_id();
  all_pass &= sigap::test::test_prototype_batch_runtime_wrong_body_hash();
  all_pass &= sigap::test::test_prototype_batch_runtime_partial_count_ack();
  all_pass &= sigap::test::test_prototype_batch_runtime_pending_body_frozen();
  all_pass &= sigap::test::test_prototype_batch_runtime_partial_reclaim_retry();
  all_pass &= sigap::test::test_prototype_batch_runtime_source_lost_before_ack();
  all_pass &= sigap::test::test_prototype_batch_runtime_source_changed_before_ack();
  all_pass &= sigap::test::test_prototype_batch_runtime_source_fault_after_partial_reclaim();
  all_pass &= sigap::test::test_prototype_batch_runtime_failed_reclaim_preserves_pending();
  all_pass &= sigap::test::test_prototype_batch_runtime_metadata_batch_counted();
  all_pass &= sigap::test::test_prototype_batch_runtime_byte_bound();
  all_pass &= sigap::test::test_sample_group_builder_byte_bound_direct();
  all_pass &= sigap::test::test_prototype_batch_runtime_bounded_accumulation();
  all_pass &= sigap::test::test_prototype_batch_runtime_unordered_unique_records();
  all_pass &= sigap::test::test_sample_group_builder_rejects_nonadjacent_duplicate();

  if (all_pass) {
    printf("\n=== ALL LIVE RUNTIME TESTS PASSED ===\n");
    return 0;
  }
  printf("\n=== SOME TESTS FAILED ===\n");
  return 1;
}
