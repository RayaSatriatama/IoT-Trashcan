// Actual production storage/codec/runtime against a host-only finite filesystem.
#define ARDUINO 1
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "storage/legacy_pending_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/runtime.h"

using namespace sigap;
using namespace sigap::storage;
using namespace sigap::telemetry_archive;
namespace {
constexpr const char* archive_path = "/sigap/archive.bin";
constexpr const char* temporary_path = "/sigap/archive.bin.migrate";
constexpr const char* backup_path = "/sigap/archive.bin.migrate.bak";
acquisition::DeviceId device() {
  acquisition::DeviceId value{};
  value.present = true; value.bytes[0] = 0x61;
  return value;
}
acquisition::BootId boot() {
  acquisition::BootId value{};
  value.present = true; value.bytes[0] = 0x71;
  return value;
}
ImmutableRecordBytes sample(uint64_t sequence) {
  acquisition::SampleRecord value{};
  value.header.device_id = device(); value.header.boot_id = boot();
  value.header.sequence = sequence;
  value.header.sensor = acquisition::SensorId::Mq4;
  value.header.stream = acquisition::StreamId::Organic;
  value.header.kind = acquisition::SampleKind::MqAdc;
  value.header.monotonic_us = sequence * 1000;
  value.header.config_ref.present = true;
  value.header.config_ref.schema_version = 1;
  value.header.config_ref.config_version = 4;
  value.quality = acquisition::SampleQuality::Ready;
  value.reason = acquisition::SampleReason::DataReady;
  value.raw.present = true; value.raw.adc_counts = 100 + sequence;
  ImmutableRecordBytes encoded{};
  assert(RecordCodec::encode_sample(value, encoded));
  assert(RecordCodec::valid(encoded));
  return encoded;
}
struct Probe {
  fs::FS* disk;
  bool known{true};
  unsigned calls{0};
  static bool read(uint64_t& out, void* context) noexcept {
    auto& self = *static_cast<Probe*>(context);
    ++self.calls;
    // Set output even on failure, proving status is authoritative.
    out = self.disk->free_bytes();
    return self.known;
  }
};
struct Fixture {
  fs::FS disk;
  LittleFsArchiveStore store{archive_path};
  Probe probe{&disk};
  std::vector<ImmutableRecordBytes> records;
  std::vector<uint8_t> original;
  Fixture() {
    assert(store.begin(disk, 65536));
    for (uint64_t i = 1; i <= 8; ++i) {
      records.push_back(sample(i));
      assert(store.append(records.back()) == AppendStatus::Durable);
    }
    original = disk.bytes(archive_path);
    assert(original.size() == store.pending_bytes());
    store.set_capacity_probe(Probe::read, &probe);
    disk.reset_mutations();
    assert(disk.active_handles == 0);
  }
  uint64_t required() const {
    return original.size() - records.front().size +
           LittleFsArchiveStore::kMigrationMetadataReserveBytes;
  }
  void free_space(uint64_t value) { disk.set_capacity(disk.used_bytes() + value); }
  bool reclaim() { return store.reclaim_pending(records[0].identity, records[0].hash); }
  void unchanged() {
    assert(disk.bytes(archive_path) == original);
    assert(store.pending_bytes() == original.size());
    assert(disk.mutations.total() == 0);
    for (const auto& value : records) assert(store.contains(value.identity, value.hash));
    assert(disk.active_handles == 0 && disk.active_reads == 0);
  }
  void compacted() {
    const std::vector<uint8_t> expected(original.begin() + records[0].size, original.end());
    assert(disk.bytes(archive_path) == expected);
    assert(store.pending_bytes() == expected.size());
    assert(!store.contains(records[0].identity, records[0].hash));
    for (std::size_t i = 1; i < records.size(); ++i)
      assert(store.contains(records[i].identity, records[i].hash));
    assert(!disk.exists(temporary_path) && !disk.exists(backup_path));
    assert(disk.active_handles == 0 && disk.active_reads == 0);
  }
};
void exact_threshold() {
  Fixture f;
  f.free_space(f.required());
  assert(f.reclaim());
  assert(f.probe.calls == 1);
  assert(f.disk.mutations.write > 0 && f.disk.mutations.rename == 2);
  f.compacted();
}
void absent_probe() {
  Fixture f;
  f.store.set_capacity_probe(nullptr, nullptr);
  assert(!f.reclaim());
  assert(f.probe.calls == 0 && f.store.health() == ArchiveHealth::Blocked);
  f.unchanged();
}
void failed_probe() {
  Fixture f;
  f.probe.known = false;
  assert(f.disk.free_bytes() > f.required());
  assert(!f.reclaim());
  assert(f.probe.calls == 1 && f.store.health() == ArchiveHealth::Blocked);
  f.unchanged();
}
void zero_free() {
  Fixture f;
  f.free_space(0);
  assert(!f.reclaim());
  assert(f.store.health() == ArchiveHealth::Blocked);
  f.unchanged();
}
void below_threshold() {
  Fixture f;
  f.free_space(f.required() - 1);
  assert(!f.reclaim());
  assert(f.probe.calls == 1 && f.store.health() == ArchiveHealth::Blocked);
  f.unchanged();
}
void repeated_then_success() {
  Fixture f;
  f.free_space(f.required() - 1);
  for (unsigned i = 0; i < 3; ++i) {
    assert(!f.reclaim());
    f.unchanged();
  }
  assert(f.probe.calls == 3);
  f.free_space(f.required());
  assert(f.reclaim());
  assert(f.probe.calls == 4);
  f.compacted();
}
void wrong_identity() {
  Fixture f;
  auto identity = f.records[0].identity;
  identity.sequence += 1000;
  assert(!f.store.reclaim_pending(identity, f.records[0].hash));
  assert(f.probe.calls == 0);
  f.unchanged();
}
void wrong_hash() {
  Fixture f;
  auto hash = f.records[0].hash;
  hash[0] ^= 1;
  assert(!f.store.reclaim_pending(f.records[0].identity, hash));
  assert(f.probe.calls == 0 && f.store.health() == ArchiveHealth::Degraded);
  f.unchanged();
}
void preserve_auxiliary() {
  Fixture f;
  const uint8_t sentinel[] = {12, 34, 56, 78};
  f.disk.append_raw(temporary_path, sentinel, sizeof(sentinel));
  f.disk.append_raw(backup_path, sentinel, sizeof(sentinel));
  const auto temporary = f.disk.bytes(temporary_path);
  const auto backup = f.disk.bytes(backup_path);
  f.free_space(0);
  f.disk.reset_mutations();
  assert(!f.reclaim());
  f.unchanged();
  assert(f.disk.bytes(temporary_path) == temporary);
  assert(f.disk.bytes(backup_path) == backup);
  assert(f.disk.mutations.open_write == 0 && f.disk.mutations.truncate == 0);
  assert(f.disk.mutations.write == 0 && f.disk.mutations.remove == 0);
  assert(f.disk.mutations.rename == 0);
}
void short_write_retry() {
  Fixture f;
  f.disk.short_write_once(5);
  assert(!f.reclaim());
  assert(f.probe.calls == 1 && f.disk.mutations.write == 1);
  assert(f.disk.bytes(archive_path) == f.original);
  assert(f.store.pending_bytes() == f.original.size());
  assert(!f.disk.exists(temporary_path) && !f.disk.exists(backup_path));
  assert(f.disk.active_handles == 0);
  assert(f.store.recover());
  assert(f.reclaim());
  f.compacted();
}
void rename_failure_retry() {
  Fixture f;
  f.disk.fail_next_rename();
  assert(!f.reclaim());
  assert(f.disk.mutations.rename == 1);
  assert(f.disk.bytes(archive_path) == f.original);
  assert(!f.disk.exists(temporary_path) && !f.disk.exists(backup_path));
  assert(f.disk.active_handles == 0);
  assert(f.store.recover());
  assert(f.reclaim());
  f.compacted();
}
void recovery_committed_replacement() {
  Fixture f;
  const std::vector<uint8_t> retained(f.original.begin() + f.records[0].size,
                                      f.original.end());
  f.disk.append_raw(temporary_path, retained.data(), retained.size());
  assert(f.disk.rename(archive_path, backup_path));
  LittleFsArchiveStore restarted(archive_path);
  assert(restarted.begin(f.disk, 65536));
  assert(f.disk.bytes(archive_path) == retained);
  assert(f.disk.bytes(backup_path) == f.original); // forensic, never resurrected
  assert(!restarted.contains(f.records[0].identity, f.records[0].hash));
  assert(restarted.pending_bytes() == retained.size());
  assert(f.disk.active_handles == 0);
}
void overflow_host_injection() {
  static_assert(sizeof(std::size_t) == sizeof(uint64_t), "64-bit host fault injection only");
  Fixture f;
  f.disk.override_size(archive_path, std::numeric_limits<std::size_t>::max());
  assert(!f.reclaim());
  assert(f.probe.calls == 1 && f.store.health() == ArchiveHealth::Blocked);
  f.disk.clear_size_override();
  f.unchanged();
}
void source_size_underflow() {
  Fixture f;
  f.disk.override_size(archive_path, f.records[0].size - 1);
  assert(!f.reclaim());
  assert(f.probe.calls == 1 && f.store.health() == ArchiveHealth::Blocked);
  f.disk.clear_size_override();
  f.unchanged();
}
void finite_media_and_alias_control() {
  fs::FS disk;
  disk.set_capacity(3);
  const uint8_t bytes[] = {1, 2, 3, 4};
  auto writer = disk.open("/control", FILE_WRITE);
  assert(writer.write(bytes, sizeof(bytes)) == 3);
  writer.close();
  assert(disk.used_bytes() == 3 && disk.free_bytes() == 0);
  auto reader = disk.open("/control", FILE_READ);
  auto alias = reader;
  assert(disk.active_reads == 1);
  reader.close();
  assert(!alias && disk.active_reads == 0 && disk.active_handles == 0);
}
struct Eligibility {
  BatchJsonBuilder builder;
  ImmutableBatch scratch;
  static bool check(const ImmutableRecordBytes& value, void* context) noexcept {
    auto& self = *static_cast<Eligibility*>(context);
    return self.builder.build(value, self.scratch);
  }
};
Ack ack_for(const ImmutableBatch& batch) {
  Ack ack{};
  ack.batch_id_size = batch.batch_id_size;
  std::memcpy(ack.batch_id.data(), batch.batch_id.data(), batch.batch_id_size);
  ack.body_hash = batch.body_hash; ack.record_count = batch.record_count;
  ack.device_id = batch.device_id; ack.stored_count = batch.record_count;
  ack.status = AckStatus::Durable;
  return ack;
}
void legacy_ack_low_space_retry() {
  fs::FS disk;
  LegacyPendingArchiveStore store;
  Probe probe{&disk};
  Eligibility eligibility;
  store.set_capacity_probe(Probe::read, &probe);
  store.set_selection_context(device(), boot(), Eligibility::check, &eligibility);
  assert(store.begin(disk, 65536));
  const auto first = sample(1), second = sample(2);
  assert(store.append(first) == AppendStatus::Durable);
  assert(store.append(second) == AppendStatus::Durable);
  const auto original = disk.bytes(archive_path);
  MemoryImmutableBatchStore<8> batches(65536);
  MemoryAckJournalStore<8> journal;
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> runtime(&store, &batches, &journal);
  assert(runtime.poll() == BatchRuntimeStatus::Durable);
  const auto ack = ack_for(runtime.batch());
  const auto body = std::string(runtime.batch().exact_body());
  disk.set_capacity(disk.used_bytes());
  disk.reset_mutations();
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    assert(runtime.apply_ack(ack) == AckPersistStatus::Retryable);
    assert(runtime.pending() && journal.size() == 1);
    const auto* receipt = journal.find_manifest(ack);
    assert(receipt && receipt->state == JournalState::ServerAcked);
    assert(store.contains(first.identity, first.hash));
    assert(disk.bytes(archive_path) == original && disk.mutations.total() == 0);
    assert(store.pending_bytes() == original.size() && disk.active_handles == 0);
    ImmutableBatch cached;
    assert(batches.load_manifest(ack, cached));
    assert(cached.exact_body() == body && cached.body_hash == ack.body_hash);
  }
  disk.set_capacity(disk.used_bytes() + second.size +
                    LittleFsArchiveStore::kMigrationMetadataReserveBytes);
  assert(runtime.apply_ack(ack) == AckPersistStatus::Durable);
  assert(!runtime.pending() && journal.size() == 0);
  assert(!store.contains(first.identity, first.hash));
  assert(store.contains(second.identity, second.hash));
  assert(disk.bytes(archive_path) == std::vector<uint8_t>(second.bytes.begin(),
                                                        second.bytes.begin() + second.size));
  assert(disk.active_handles == 0);
}
}  // namespace
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  if (scenario == "exact_threshold") exact_threshold();
  else if (scenario == "absent_probe") absent_probe();
  else if (scenario == "failed_probe") failed_probe();
  else if (scenario == "zero_free") zero_free();
  else if (scenario == "below_threshold") below_threshold();
  else if (scenario == "repeated_then_success") repeated_then_success();
  else if (scenario == "wrong_identity") wrong_identity();
  else if (scenario == "wrong_hash") wrong_hash();
  else if (scenario == "preserve_auxiliary") preserve_auxiliary();
  else if (scenario == "short_write_retry") short_write_retry();
  else if (scenario == "rename_failure_retry") rename_failure_retry();
  else if (scenario == "recovery_committed_replacement") recovery_committed_replacement();
  else if (scenario == "overflow_host_injection") overflow_host_injection();
  else if (scenario == "source_size_underflow") source_size_underflow();
  else if (scenario == "finite_media_and_alias_control") finite_media_and_alias_control();
  else if (scenario == "legacy_ack_low_space_retry") legacy_ack_low_space_retry();
  else { std::fprintf(stderr, "Unknown scenario: %s\n", argv[1]); return 2; }
  std::printf("CASE %s PASS\n", argv[1]);
  return 0;
}
