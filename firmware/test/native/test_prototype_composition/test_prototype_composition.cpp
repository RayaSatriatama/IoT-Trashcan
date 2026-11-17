#include "acquisition/sequence.h"
#include "prototype_live_runtime.h"
#include "archive_runtime.h"
#include "storage/archive_store.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace {
using namespace sigap;

acquisition::SampleRecord sample_for(const acquisition::DeviceId& device,
                                     const acquisition::BootId& boot,
                                     const acquisition::ConfigRef& config,
                                     uint64_t sequence) {
  acquisition::SampleRecord sample{};
  sample.header.device_id = device;
  sample.header.boot_id = boot;
  sample.header.sequence = sequence;
  sample.header.monotonic_us = sequence * 1000U;
  sample.header.config_ref = config;
  sample.header.sensor = acquisition::SensorId::Hx711Organic;
  sample.header.stream = acquisition::StreamId::Organic;
  sample.header.kind = acquisition::SampleKind::Hx711Counts;
  sample.quality = acquisition::SampleQuality::Uncalibrated;
  sample.reason = acquisition::SampleReason::ProfileMissing;
  sample.raw.present = true;
  sample.raw.signed_counts = static_cast<int64_t>(sequence * 101U);
  return sample;
}

acquisition::SampleRecord ultrasonic_sample_for(const acquisition::DeviceId& device,
                                                const acquisition::BootId& boot,
                                                const acquisition::ConfigRef& config,
                                                uint64_t sequence) {
  auto sample = sample_for(device, boot, config, sequence);
  sample.header.sensor = acquisition::SensorId::UltrasonicOrganic;
  sample.header.stream = acquisition::StreamId::Organic;
  sample.header.kind = acquisition::SampleKind::UltrasonicEcho;
  sample.quality = acquisition::SampleQuality::Ready;
  sample.converted.primary_available = true;
  sample.converted.primary_kind = acquisition::ConvertedKind::FillPercent;
  sample.converted.primary_value = sequence == 100U ? 0.0F : 47.0F;
  sample.converted.secondary_available = false;
  return sample;
}

bool check(bool condition, const char* message) {
  if (!condition) std::printf("[FAIL] %s\n", message);
  return condition;
}

telemetry_archive::Ack ack_for(const telemetry_archive::ImmutableBatch& batch,
                               const acquisition::DeviceId& device) {
  telemetry_archive::Ack ack{};
  ack.device_id = device;
  ack.batch_id_size = batch.batch_id_size;
  std::copy_n(batch.batch_id.begin(), batch.batch_id_size, ack.batch_id.begin());
  ack.body_hash = batch.body_hash;
  ack.record_count = batch.record_count;
  ack.stored_count = batch.record_count;
  ack.status = telemetry_archive::AckStatus::Durable;
  return ack;
}

bool run() {
  const auto assert_fill = [](uint64_t sequence, const char* expected) {
    storage::MemoryArchiveStore<2U> fill_store(64U * 1024U);
    ArchiveRuntime fill_runtime(&fill_store);
    acquisition::SequenceAllocator fill_sequence{sequence};
    fill_runtime.set_sequence_allocator(&fill_sequence);
    acquisition::DeviceId fill_device{};
    fill_device.present = true;
    fill_device.bytes.fill(0xA1U);
    acquisition::BootId fill_boot{};
    fill_boot.present = true;
    fill_boot.bytes.fill(0xB2U);
    acquisition::ConfigRef fill_config{};
    fill_config.present = true;
    fill_config.schema_version = 1U;
    fill_config.config_version = 1U;
    fill_config.hash.fill(0xC3U);
    fill_runtime.set_event_context(fill_device, fill_boot, fill_config);
    PrototypeLiveBatchRuntime fill_batch;
    fill_batch.bind(&fill_store);
    if (fill_runtime.append_sample(ultrasonic_sample_for(fill_device, fill_boot, fill_config, sequence)) !=
        storage::AppendStatus::Queued) return false;
    fill_runtime.poll_storage_slice(4U, 32768U);
    if (fill_batch.poll() != telemetry_archive::BatchRuntimeStatus::Durable) return false;
    const std::string_view body(
        reinterpret_cast<const char*>(fill_batch.batch().body.data()),
        fill_batch.batch().body_size);
    return body.find(expected) != std::string_view::npos;
  };
  if (!check(assert_fill(100U, "\"converted\":{\"distance_mm\":null,\"fill_percent\":0}"),
             "ultrasonic zero fill is encoded as numeric zero")) return false;
  if (!check(assert_fill(101U, "\"converted\":{\"distance_mm\":null,\"fill_percent\":47}"),
             "ultrasonic finite fill is encoded without fabricated distance")) return false;

  storage::MemoryArchiveStore<16U> store(64U * 1024U);
  ArchiveRuntime runtime(&store);
  acquisition::SequenceAllocator sequence{100U};
  runtime.set_sequence_allocator(&sequence);
  acquisition::DeviceId device{};
  device.present = true;
  device.bytes.fill(0xA1U);
  acquisition::BootId boot{};
  boot.present = true;
  boot.bytes.fill(0xB2U);
  acquisition::ConfigRef config{};
  config.present = true;
  config.schema_version = 1U;
  config.config_version = 1U;
  config.hash.fill(0xC3U);
  runtime.set_event_context(device, boot, config);

  PrototypeLiveBatchRuntime batch;
  batch.bind(&store);
  batch.bind_debug(nullptr, nullptr);

  if (!check(runtime.append_sample(sample_for(device, boot, config, 101U)) ==
                 storage::AppendStatus::Queued && store.size() == 0U &&
                 runtime.queued_records() == 1U,
             "append_sample queues without direct store append")) return false;
  if (!check(batch.poll() == telemetry_archive::BatchRuntimeStatus::Idle,
             "unpersisted queue is not batch-visible")) return false;
  runtime.poll_storage_slice(4U, 32768U);
  if (!check(store.size() == 1U && runtime.queued_records() == 0U,
             "storage slice persists queued sample")) return false;
  if (!check(batch.poll() == telemetry_archive::BatchRuntimeStatus::Durable &&
                 batch.pending() && batch.batch().body_size != 0U &&
                 std::any_of(batch.batch().body_hash.begin(),
                             batch.batch().body_hash.end(),
                             [](uint8_t byte) { return byte != 0U; }),
             "batch runtime creates pending body")) return false;
  const auto retained_id = batch.batch().batch_id;
  const auto retained_hash = batch.batch().body_hash;
  auto forged = ack_for(batch.batch(), device);
  forged.device_id.present = false;
  if (!check(batch.apply_ack(forged) == telemetry_archive::AckPersistStatus::Invalid &&
                 batch.pending() && store.size() == 1U &&
                 batch.batch().batch_id == retained_id &&
                 batch.batch().body_hash == retained_hash,
             "invalid ACK leaves source and body intact")) return false;
  if (!check(batch.apply_ack(ack_for(batch.batch(), device)) ==
                 telemetry_archive::AckPersistStatus::Durable &&
                 !batch.pending() && store.size() == 0U,
             "exact ACK reclaims source")) return false;

  for (uint64_t cycle = 102U; cycle < 122U; ++cycle) {
    if (!check(runtime.append_sample(sample_for(device, boot, config, cycle)) ==
                   storage::AppendStatus::Queued,
               "repeated sample append queues")) return false;
    runtime.poll_storage_slice(4U, 32768U);
    if (!check(store.size() == 1U && runtime.queued_records() == 0U &&
                   batch.poll() == telemetry_archive::BatchRuntimeStatus::Durable &&
                   batch.pending(),
               "repeated composition reaches pending batch")) return false;
    if (!check(batch.apply_ack(ack_for(batch.batch(), device)) ==
                   telemetry_archive::AckPersistStatus::Durable &&
                   !batch.pending() && store.size() == 0U,
               "repeated exact ACK reclaims only after batch")) return false;
  }

  if (!check(runtime.append_boot_metadata(25000U, "test-fw", 1U) ==
                 storage::AppendStatus::Queued,
             "boot metadata enters ArchiveRuntime queue")) return false;
  runtime.poll_storage_slice(4U, 32768U);
  if (!check(batch.poll() == telemetry_archive::BatchRuntimeStatus::Durable &&
                 batch.pending(), "boot metadata becomes a batch")) return false;
  if (!check(batch.apply_ack(ack_for(batch.batch(), device)) ==
                 telemetry_archive::AckPersistStatus::Durable && store.size() == 0U,
             "boot metadata exact ACK reclaims it")) return false;
  if (!check(runtime.append_sample(sample_for(device, boot, config, 122U)) ==
                 storage::AppendStatus::Queued,
             "sample advances after boot metadata ACK")) return false;
  runtime.poll_storage_slice(4U, 32768U);
  if (!check(store.size() == 1U && batch.poll() ==
                   telemetry_archive::BatchRuntimeStatus::Durable &&
                   batch.pending(), "post-boot sample is batchable")) return false;
  if (!check(batch.apply_ack(ack_for(batch.batch(), device)) ==
                 telemetry_archive::AckPersistStatus::Durable &&
                 store.size() == 0U,
             "post-boot sample exact ACK reclaims it")) return false;

  // Multi-sample composition from the real ArchiveRuntime queue: three queued
  // samples must reach one frozen body and one whole-batch ACK.
  for (uint64_t queued = 123U; queued < 126U; ++queued) {
    if (!check(runtime.append_sample(sample_for(device, boot, config, queued)) ==
                   storage::AppendStatus::Queued,
               "multi-sample append queues")) return false;
    runtime.poll_storage_slice(4U, 32768U);
  }
  if (!check(store.size() == 3U &&
                 batch.poll() == telemetry_archive::BatchRuntimeStatus::Durable &&
                 batch.pending() && batch.batch().record_count == 3U,
             "three queued samples compose one multi-record batch")) return false;
  const std::string_view grouped(
      reinterpret_cast<const char*>(batch.batch().body.data()),
      batch.batch().body_size);
  if (!check(grouped.find("\"record_count\":3,\"samples\":[") !=
                 std::string_view::npos,
             "multi-record body declares the exact record count")) return false;
  return check(batch.apply_ack(ack_for(batch.batch(), device)) ==
                   telemetry_archive::AckPersistStatus::Durable &&
                   !batch.pending() && store.size() == 0U,
               "multi-record exact ACK reclaims every frozen source");
}
}  // namespace

int main() {
  return run() ? 0 : 1;
}
