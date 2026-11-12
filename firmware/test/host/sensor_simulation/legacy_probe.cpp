// Filesystem-shaped host proof only. No board, production DB, or live endpoint.
#define ARDUINO 1
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include "storage/legacy_pending_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/runtime.h"

using namespace sigap;
using namespace sigap::storage;
using namespace sigap::telemetry_archive;
namespace {
acquisition::DeviceId device() {
  acquisition::DeviceId v{}; v.present = true; v.bytes[0] = 0x61U; return v;
}
acquisition::BootId boot(uint8_t tag) {
  acquisition::BootId v{}; v.present = true; v.bytes[0] = tag; return v;
}
struct CapacityFixture { uint64_t available{65536U}; bool known{true}; };
bool capacity(uint64_t& out, void* context) noexcept {
  const auto& fixture = *static_cast<const CapacityFixture*>(context);
  out = fixture.available; return fixture.known;
}
CapacityFixture& synthetic_capacity() { static CapacityFixture fixture{}; return fixture; }
struct Eligibility {
  BatchJsonBuilder builder{};
  ImmutableBatch scratch{};
  unsigned calls{0};
  static bool check(const ImmutableRecordBytes& r, void* context) noexcept {
    auto& self = *static_cast<Eligibility*>(context);
    ++self.calls;
    return self.builder.build(r, self.scratch);
  }
};
ImmutableRecordBytes sample(uint64_t sequence, uint8_t boot_tag,
                            bool missing_phase = false, bool old_layout = false,
                            bool sht31 = false) {
  acquisition::SampleRecord s{};
  s.header.device_id = device(); s.header.boot_id = boot(boot_tag);
  s.header.sequence = sequence;
  s.header.sensor = missing_phase ? acquisition::SensorId::Mq7 : acquisition::SensorId::Mq4;
  s.header.stream = missing_phase ? acquisition::StreamId::B3 : acquisition::StreamId::Organic;
  s.header.kind = acquisition::SampleKind::MqAdc;
  s.header.monotonic_us = sequence * 1000U;
  s.header.config_ref.present = true;
  s.header.config_ref.schema_version = 1U; s.header.config_ref.config_version = 4U;
  s.raw.present = true; s.raw.adc_counts = 123U;
  if (sht31) {
    s.header.sensor = acquisition::SensorId::Sht31_44;
    s.header.kind = acquisition::SampleKind::Sht31;
    s.raw.sht31_ticks_available = true;
    s.raw.sht31_temperature_ticks = 20000U;
    s.raw.sht31_humidity_ticks = 30000U;
    s.raw.temperature_crc_valid = s.raw.humidity_crc_valid = s.raw.crc_valid = true;
    s.raw.temperature_centi_c = static_cast<int32_t>((-4500LL * 65535LL + 17500LL * 20000LL) / 65535LL);
    s.raw.humidity_centi_pct = static_cast<int32_t>(10000LL * 30000LL / 65535LL);
  }
  s.quality = acquisition::SampleQuality::Ready;
  s.reason = acquisition::SampleReason::DataReady;
  ImmutableRecordBytes encoded{};
  assert(RecordCodec::encode_sample(s, encoded));
  if (!old_layout) return encoded;
  // Synthetic pre-extension fixture: retain precisely the original sample payload
  // layout and rebuild only its actual archive frame CRC/hash using the codec.
  const std::size_t payload = encoded.size - kArchiveHeaderBytes -
                              kArchiveTrailerBytes - kArchiveSampleExtensionBytes;
  ImmutableRecordBytes legacy{};
  PayloadWriter writer{nullptr, 0U};
  assert(RecordCodec::begin(encoded.identity, legacy, writer));
  assert(writer.bytes(encoded.bytes.data() + kArchiveHeaderBytes, payload));
  assert(RecordCodec::finish(legacy, writer));
  assert(RecordCodec::valid(legacy));
  return legacy;
}
void append(LegacyPendingArchiveStore& store, const ImmutableRecordBytes& record) {
  assert(store.append(record) == AppendStatus::Durable);
}
Ack ack_for(const ImmutableBatch& b) {
  Ack a{}; a.batch_id_size = b.batch_id_size;
  std::memcpy(a.batch_id.data(), b.batch_id.data(), b.batch_id_size);
  a.body_hash = b.body_hash; a.record_count = b.record_count;
  a.device_id = b.device_id; a.stored_count = b.record_count; a.status = AckStatus::Durable;
  return a;
}
void retain_and_ack() {
  fs::FS disk; Eligibility eligible;
  LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto old = sample(1U, 1U, true, true), fresh = sample(2U, 2U);
  assert(!Eligibility::check(old, &eligible));
  assert(Eligibility::check(fresh, &eligible));
  append(store, old); append(store, fresh);
  const auto before = disk.bytes("/sigap/archive.bin");
  MemoryImmutableBatchStore<8U> batches(65536U);
  MemoryAckJournalStore<8U> journal;
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> runtime(&store, &batches, &journal);
  assert(runtime.poll() == BatchRuntimeStatus::Durable);
  assert(disk.bytes("/sigap/archive.bin") == before);
  assert(store.retained_records() == 1U && store.retained_bytes() == old.size);
  auto invalid = ack_for(runtime.batch()); invalid.body_hash[0] ^= 1U;
  assert(runtime.apply_ack(invalid) != AckPersistStatus::Durable);
  assert(disk.bytes("/sigap/archive.bin") == before);
  assert(runtime.apply_ack(ack_for(runtime.batch())) == AckPersistStatus::Durable);
  assert(disk.bytes("/sigap/archive.bin") == std::vector<uint8_t>(old.bytes.begin(), old.bytes.begin() + old.size));
  ImmutableRecordBytes out{}; assert(!store.peek_pending(out));
  assert(store.pending_bytes() == old.size && store.health() == ArchiveHealth::Degraded);
  assert(!store.reclaim_pending(old.identity, old.hash));
}
void current_fault() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto bad = sample(1U, 2U, true), good = sample(2U, 2U);
  append(store, bad); append(store, good);
  const auto before = disk.bytes("/sigap/archive.bin");
  ImmutableRecordBytes out{}; assert(store.peek_pending(out));
  assert(out.hash == bad.hash && !Eligibility::check(out, &eligible));
  assert(store.retained_records() == 0U && store.selection_faulted());
  assert(!store.reclaim_pending(bad.identity, bad.hash));
  assert(disk.bytes("/sigap/archive.bin") == before);
}
void bounded_and_restart() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  for (uint64_t i = 1U; i <= LegacyPendingArchiveStore::kRecordsPerPeek + 1U; ++i)
    append(store, sample(i, 1U, true, true));
  auto good = sample(100U, 1U); append(store, good); // Old does not mean unsupported.
  const auto before = disk.bytes("/sigap/archive.bin");
  ImmutableRecordBytes out{}; const auto calls = eligible.calls;
  assert(!store.peek_pending(out));
  assert(eligible.calls - calls <= LegacyPendingArchiveStore::kRecordsPerPeek);
  assert(store.peek_pending(out) && out.hash == good.hash);
  assert(store.peek_pending(out) && out.hash == good.hash); // Stable until ACK.
  LegacyPendingArchiveStore restarted;
  restarted.set_capacity_probe(capacity, &synthetic_capacity());
  restarted.set_selection_context(device(), boot(3U), Eligibility::check, &eligible);
  assert(restarted.begin(disk, 65536U));
  assert(!restarted.peek_pending(out)); assert(restarted.peek_pending(out));
  assert(out.hash == good.hash);
  assert(restarted.reclaim_pending(good.identity, good.hash));
  assert(store.pending_bytes() > 0U); // This old instance is not a drain oracle.
  assert(disk.bytes("/sigap/archive.bin").size() == before.size() - good.size);
}
void io_fault_and_quota() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  auto old = sample(1U, 1U, true, true);
  assert(store.begin(disk, old.size)); append(store, old);
  const auto before = disk.bytes("/sigap/archive.bin");
  assert(store.append(sample(2U, 2U)) == AppendStatus::LoggingBlocked);
  ImmutableRecordBytes out{}; disk.short_read_once(1U);
  assert(!store.peek_pending(out)); assert(store.selection_faulted());
  assert(disk.bytes("/sigap/archive.bin") == before);
  disk.clear_faults(); assert(store.recover()); assert(!store.peek_pending(out));
  assert(store.pending_bytes() == old.size && store.retained_bytes() == old.size);
}
void sht_ticks_are_not_reconstructed() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto old = sample(1U, 1U, false, true, true);
  auto modern = sample(2U, 2U, false, false, true);
  assert(!Eligibility::check(old, &eligible) && Eligibility::check(modern, &eligible));
  append(store, old); append(store, modern);
  ImmutableRecordBytes out{};
  assert(store.peek_pending(out) && out.hash == modern.hash);
  assert(store.retained_bytes() == old.size);
  assert(store.reclaim_pending(modern.identity, modern.hash));
  const auto bytes = disk.bytes("/sigap/archive.bin");
  assert(bytes == std::vector<uint8_t>(old.bytes.begin(), old.bytes.begin() + old.size));
}
void ack_after_restart_without_prior_selection() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto old = sample(1U, 1U, true, true), good = sample(2U, 2U);
  append(store, old); append(store, good);
  MemoryImmutableBatchStore<8U> batches(65536U); MemoryAckJournalStore<8U> journal;
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> first(&store, &batches, &journal);
  assert(first.poll() == BatchRuntimeStatus::Durable);
  const auto ack = ack_for(first.batch());
  LegacyPendingArchiveStore restarted;
  restarted.set_capacity_probe(capacity, &synthetic_capacity());
  restarted.set_selection_context(device(), boot(3U), Eligibility::check, &eligible);
  assert(restarted.begin(disk, 65536U));
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> second(&restarted, &batches, &journal);
  assert(second.apply_ack(ack) == AckPersistStatus::Durable);
  assert(disk.bytes("/sigap/archive.bin") == std::vector<uint8_t>(old.bytes.begin(), old.bytes.begin() + old.size));
  assert(!restarted.reclaim_pending(old.identity, old.hash));
}
void null_archive_ack_stays_retryable() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto old = sample(1U, 1U, true, true), good = sample(2U, 2U);
  append(store, old); append(store, good);
  const auto before = disk.bytes("/sigap/archive.bin");
  MemoryImmutableBatchStore<8U> batches(65536U); MemoryAckJournalStore<8U> journal;
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> runtime(&store, &batches, &journal);
  assert(runtime.poll() == BatchRuntimeStatus::Durable);
  const auto ack = ack_for(runtime.batch());
  runtime.bind(nullptr, &batches, &journal);
  assert(runtime.poll() == BatchRuntimeStatus::Retryable);
  assert(runtime.pending());
  assert(runtime.apply_ack(ack) == AckPersistStatus::Retryable);
  assert(journal.size() == 1U && runtime.pending());
  assert(journal.at(0U)->state == JournalState::Pending);
  assert(disk.bytes("/sigap/archive.bin") == before);
  ImmutableBatch loaded{};
  assert(batches.load_manifest(ack, loaded));
  assert(loaded.exact_body() == runtime.batch().exact_body());
  runtime.bind(&store, &batches, &journal);
  assert(runtime.apply_ack(ack) == AckPersistStatus::Durable);
  assert(journal.size() == 0U);
  assert(disk.bytes("/sigap/archive.bin") == std::vector<uint8_t>(old.bytes.begin(), old.bytes.begin() + old.size));
}
void null_archive_ack_after_restart_stays_retryable() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto old = sample(1U, 1U, true, true), good = sample(2U, 2U);
  append(store, old); append(store, good);
  const auto before = disk.bytes("/sigap/archive.bin");
  MemoryImmutableBatchStore<8U> batches(65536U); MemoryAckJournalStore<8U> journal;
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> first(&store, &batches, &journal);
  assert(first.poll() == BatchRuntimeStatus::Durable);
  const auto ack = ack_for(first.batch());
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> restarted(nullptr, &batches, &journal);
  assert(restarted.apply_ack(ack) == AckPersistStatus::Retryable);
  assert(journal.size() == 1U && disk.bytes("/sigap/archive.bin") == before);
  assert(journal.at(0U)->state == JournalState::Pending);
  ImmutableBatch loaded{};
  assert(batches.load_manifest(ack, loaded));
  assert(loaded.exact_body() == first.batch().exact_body());
  restarted.bind(&store, &batches, &journal);
  assert(restarted.apply_ack(ack) == AckPersistStatus::Durable);
  assert(journal.size() == 0U);
  assert(disk.bytes("/sigap/archive.bin") == std::vector<uint8_t>(old.bytes.begin(), old.bytes.begin() + old.size));
}
void cached_unsupported_manifest_unchanged() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto old = sample(1U, 1U, true, true); append(store, old); append(store, sample(2U, 2U));
  MemoryImmutableBatchStore<8U> batches(65536U); MemoryAckJournalStore<8U> journal;
  ImmutableBatch faulted{};
  constexpr char body[] = "unsupported-immutable-test-manifest";
  assert(faulted.prepare("retained-faulted-test", reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1U, 1U));
  faulted.set_device_id(device());
  assert(faulted.bind_source(device(), boot(1U), 1U,
      static_cast<uint8_t>(ArchiveRecordKind::Sample), old.hash));
  assert(batches.append(faulted) == BatchStoreStatus::Durable);
  assert(journal.append_pending(faulted) == AckPersistStatus::Durable);
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> runtime(&store, &batches, &journal);
  assert(runtime.poll() == BatchRuntimeStatus::Durable);
  assert(runtime.apply_ack(ack_for(runtime.batch())) == AckPersistStatus::Durable);
  for (uint64_t sequence = 3U; sequence <= 5U; ++sequence) {
    append(store, sample(sequence, 2U));
    assert(runtime.poll() == BatchRuntimeStatus::Durable);
    assert(runtime.apply_ack(ack_for(runtime.batch())) == AckPersistStatus::Durable);
    LegacyPendingArchiveStore restarted;
    restarted.set_capacity_probe(capacity, &synthetic_capacity());
  restarted.set_selection_context(device(), boot(static_cast<uint8_t>(sequence + 2U)),
                                   Eligibility::check, &eligible);
    assert(restarted.begin(disk, 65536U));
    // A restart must not reinterpret or rewrite the original immutable faulted manifest.
    ImmutableBatch loaded{};
    assert(batches.load_manifest(ack_for(faulted), loaded));
    assert(loaded.exact_body() == faulted.exact_body() && loaded.body_hash == faulted.body_hash);
    assert(restarted.contains(old.identity, old.hash));
    assert(!restarted.reclaim_pending(old.identity, old.hash));
  }
  ImmutableBatch loaded{};
  assert(batches.load_manifest(ack_for(faulted), loaded));
  assert(loaded.exact_body() == faulted.exact_body() && loaded.body_hash == faulted.body_hash);
  assert(store.contains(old.identity, old.hash) && !store.reclaim_pending(old.identity, old.hash));
}
void invalid_or_foreign_identity_not_legacy() {
  for (bool foreign : {false, true}) {
    fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
    store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
    assert(store.begin(disk, 65536U));
    auto old = sample(1U, 1U, true, true);
    ArchiveRecordView view{}; assert(RecordCodec::read(old, view));
    auto identity = old.identity;
    if (foreign) identity.device_id.bytes[0] ^= 0x80U;
    else identity.boot_id.bytes.fill(0U);
    ImmutableRecordBytes changed{}; PayloadWriter writer{nullptr, 0U};
    assert(RecordCodec::begin(identity, changed, writer));
    assert(writer.bytes(view.payload, view.payload_size)); assert(RecordCodec::finish(changed, writer));
    append(store, changed); append(store, sample(2U, 2U));
    const auto before = disk.bytes("/sigap/archive.bin"); ImmutableRecordBytes out{};
    assert(!store.peek_pending(out));
    assert(store.retained_records() == 0U && store.selection_faulted());
    assert(disk.bytes("/sigap/archive.bin") == before);
  }
}
void canonical_bad_identity_faults_before_eligibility() {
  for (bool foreign : {false, true}) {
    fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
    store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
    assert(store.begin(disk, 65536U));
    auto record = sample(1U, 1U);
    ArchiveRecordView view{}; assert(RecordCodec::read(record, view));
    auto identity = record.identity;
    if (foreign) identity.device_id.bytes[0] ^= 0x80U;
    else identity.boot_id.bytes.fill(0U);
    ImmutableRecordBytes changed{}; PayloadWriter writer{nullptr, 0U};
    assert(RecordCodec::begin(identity, changed, writer));
    assert(writer.bytes(view.payload, view.payload_size));
    assert(RecordCodec::finish(changed, writer) && RecordCodec::valid(changed));
    assert(Eligibility::check(changed, &eligible)); // canonical positive control
    append(store, changed); append(store, sample(2U, 2U));
    const auto before = disk.bytes("/sigap/archive.bin");
    MemoryImmutableBatchStore<8U> batches(65536U); MemoryAckJournalStore<8U> journal;
    ArchiveBatchRuntime<decltype(batches), decltype(journal)> runtime(&store, &batches, &journal);
    assert(runtime.poll() != BatchRuntimeStatus::Durable);
    ImmutableRecordBytes out{}; assert(!store.peek_pending(out));
    assert(store.selection_faulted());
    assert(store.retained_records() == 0U && !store.reclaim_pending(changed.identity, changed.hash));
    assert(disk.bytes("/sigap/archive.bin") == before);
    assert(batches.size() == 0U && journal.reclaim() == 0U);
  }
}
void retained_interleaving_append_ack_restart() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(4U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  auto r1=sample(1U,1U,true,true), e1=sample(2U,4U), r2=sample(3U,1U,true,true), e2=sample(4U,4U);
  append(store,r1); append(store,e1); append(store,r2);
  MemoryImmutableBatchStore<8U> batches(65536U); MemoryAckJournalStore<8U> journal;
  ArchiveBatchRuntime<decltype(batches), decltype(journal)> runtime(&store,&batches,&journal);
  assert(runtime.poll()==BatchRuntimeStatus::Durable);
  auto first=runtime.batch(); assert(first.record_count==1U && first.source.sequence==e1.identity.sequence);
  assert(runtime.apply_ack(ack_for(first))==AckPersistStatus::Durable);
  append(store,e2); // file compaction/reclaim must preserve the retained prefix
  assert(store.recover());
  assert(runtime.poll()==BatchRuntimeStatus::Idle); // two retained prefix records
  assert(runtime.poll()==BatchRuntimeStatus::Durable);
  auto second=runtime.batch(); assert(second.source.sequence==e2.identity.sequence);
  assert(runtime.apply_ack(ack_for(second))==AckPersistStatus::Durable);
  append(store,sample(5U,4U));
  LegacyPendingArchiveStore restarted; restarted.set_capacity_probe(capacity, &synthetic_capacity()); restarted.set_selection_context(device(),boot(5U),Eligibility::check,&eligible);
  assert(restarted.begin(disk,65536U));
  MemoryImmutableBatchStore<8U> batches2(65536U); MemoryAckJournalStore<8U> journal2;
  ArchiveBatchRuntime<decltype(batches2),decltype(journal2)> after(&restarted,&batches2,&journal2);
  assert(after.poll()==BatchRuntimeStatus::Idle); // retained prefix is still first
  assert(after.poll()==BatchRuntimeStatus::Durable && after.batch().source.sequence==5U);
  const auto original=disk.bytes("/sigap/archive.bin");
  assert(after.apply_ack(ack_for(after.batch()))==AckPersistStatus::Durable);
  const auto remaining=disk.bytes("/sigap/archive.bin");
  assert(remaining.size()==r1.size+r2.size);
  assert(std::memcmp(remaining.data(),r1.bytes.data(),r1.size)==0);
  assert(std::memcmp(remaining.data()+r1.size,r2.bytes.data(),r2.size)==0);
  assert(restarted.contains(r1.identity,r1.hash) && restarted.contains(r2.identity,r2.hash));
  assert(original.size()==remaining.size()+sample(5U,4U).size);
}
void historical_malformed_result_is_retained() {
  fs::FS disk; Eligibility eligible; LegacyPendingArchiveStore store;
  store.set_capacity_probe(capacity, &synthetic_capacity());
  store.set_selection_context(device(), boot(2U), Eligibility::check, &eligible);
  assert(store.begin(disk, 65536U));
  ImmutableRecordBytes result{}; PayloadWriter writer{nullptr, 0U};
  assert(RecordCodec::begin({device(), boot(1U), 1U, ArchiveRecordKind::Result}, result, writer));
  assert(writer.u8(0U) && RecordCodec::finish(result, writer));
  assert(!Eligibility::check(result, &eligible)); append(store, result);
  auto good = sample(2U, 2U); append(store, good); ImmutableRecordBytes out{};
  assert(store.peek_pending(out) && out.hash == good.hash);
  assert(store.reclaim_pending(good.identity, good.hash));
  assert(disk.bytes("/sigap/archive.bin") == std::vector<uint8_t>(result.bytes.begin(), result.bytes.begin() + result.size));
}
void emit_fixtures() {
  const auto mq = sample(1U, 1U, true, true);
  const auto sht = sample(2U, 1U, false, true, true);
  std::printf("{\"provenance\":\"synthetic pre-extension payloads, not captured hardware\",\"frames\":[");
  bool first = true;
  for (const auto* record : {&mq, &sht}) {
    if (!first) std::printf(",");
    first = false;
    std::printf("{\"hex\":\"");
    for (std::size_t i = 0U; i < record->size; ++i) std::printf("%02x", record->bytes[i]);
    std::printf("\"}");
  }
  std::printf("]}\n");
}
}
int main(int argc, char** argv) {
  assert(argc == 2); const std::string scenario(argv[1]);
  if (scenario == "retain_ack") retain_and_ack();
  else if (scenario == "current_fault") current_fault();
  else if (scenario == "bounded_restart") bounded_and_restart();
  else if (scenario == "io_quota") io_fault_and_quota();
  else if (scenario == "sht_ticks") sht_ticks_are_not_reconstructed();
  else if (scenario == "ack_restart") ack_after_restart_without_prior_selection();
  else if (scenario == "null_archive") null_archive_ack_stays_retryable();
  else if (scenario == "null_archive_restart") null_archive_ack_after_restart_stays_retryable();
  else if (scenario == "cached_manifest") cached_unsupported_manifest_unchanged();
  else if (scenario == "identity_guards") invalid_or_foreign_identity_not_legacy();
  else if (scenario == "historical_result") historical_malformed_result_is_retained();
  else if (scenario == "canonical_identity") canonical_bad_identity_faults_before_eligibility();
  else if (scenario == "interleaving") retained_interleaving_append_ack_restart();
  else if (scenario == "emit_fixtures") emit_fixtures();
  else return 2;
}
