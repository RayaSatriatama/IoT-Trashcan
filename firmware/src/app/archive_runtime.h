#pragma once

#include <cstddef>
#include <cstdint>

#include "acquisition/record.h"
#include "processing_runtime.h"
#include "storage/archive_store.h"
#include "telemetry_archive/batch_store.h"

namespace sigap {

class ArchiveRuntime final : public acquisition::ISampleObserver {
 public:
  using ResultEnvelope = ProcessingRuntime::ResultEnvelope;
  using ResultHandoff = ProcessingRuntime::Handoff;

  explicit ArchiveRuntime(storage::IArchiveStore* store = nullptr) noexcept
      : store_(store) {}

  void set_store(storage::IArchiveStore* store) noexcept {
    store_ = store;
  }
  void set_secondary_observer(acquisition::ISampleObserver* observer) noexcept {
    secondary_ = observer;
  }

  void observe(const acquisition::SampleRecord& sample) noexcept override {
    if (secondary_ != nullptr) secondary_->observe(sample);
    storage::ImmutableRecordBytes encoded{};
    if (!storage::RecordCodec::encode_sample(sample, encoded)) {
      ++encode_failures_;
      gaps_.record(sample.header.sequence, storage::GapReason::InvalidRecord);
      return;
    }
    (void)append_record(encoded);
  }

  storage::AppendStatus append_record(
      const storage::ImmutableRecordBytes& record) noexcept {
    return append(record);
  }

  bool drain_results(ResultHandoff& handoff) noexcept {
    bool progressed{false};
    ResultEnvelope envelope{};
    while (handoff.try_peek(envelope) == processing::ResultHandoffStatus::Dequeued) {
      storage::ImmutableRecordBytes encoded{};
      if (!encode_result(envelope, encoded)) {
        ++encode_failures_;
        handoff.discard_front();
        gaps_.record(envelope.header.sequence, storage::GapReason::InvalidRecord);
        progressed = true;
        continue;
      }
      if (append(encoded) != storage::AppendStatus::Durable) break;
      handoff.discard_front();
      progressed = true;
    }
    return progressed;
  }

  uint64_t durable_records() const noexcept { return durable_records_; }
  uint64_t encode_failures() const noexcept { return encode_failures_; }
  const storage::GapTracker<32U>& gaps() const noexcept { return gaps_; }
  storage::ArchiveHealth health() const noexcept {
    return store_ == nullptr ? storage::ArchiveHealth::Degraded : store_->health();
  }

 private:
  storage::AppendStatus append(const storage::ImmutableRecordBytes& record) noexcept {
    if (store_ == nullptr) {
      gaps_.record(record.identity.sequence, storage::GapReason::MediaFailure);
      return storage::AppendStatus::Retryable;
    }
    const auto status = store_->append(record);
    if (status == storage::AppendStatus::Durable) ++durable_records_;
    else gaps_.record(record.identity.sequence,
                      status == storage::AppendStatus::LoggingBlocked
                          ? storage::GapReason::QuotaFull
                          : storage::GapReason::MediaFailure);
    return status;
  }

  bool encode_result(const ResultEnvelope& envelope,
                     storage::ImmutableRecordBytes& output) noexcept {
    const storage::ArchiveIdentity identity{
        envelope.header.device_id, envelope.header.boot_id,
        envelope.header.sequence, storage::ArchiveRecordKind::Result};
    storage::PayloadWriter writer{nullptr, 0U};
    if (!storage::RecordCodec::begin(identity, output, writer)) return false;
    writer = storage::PayloadWriter(
        output.bytes.data() + storage::kArchiveHeaderBytes,
        output.bytes.size() - storage::kArchiveHeaderBytes - storage::kArchiveTrailerBytes);
    const auto& processed = envelope.result.processing;
    if (!writer.u8(static_cast<uint8_t>(processed.stream)) ||
        !writer.u64(processed.computed_monotonic_us) ||
        !writer.u8(envelope.header.recorded_at.available ? 1U : 0U) ||
        !writer.i64(envelope.header.recorded_at.epoch_ms) ||
        !writer.u8(static_cast<uint8_t>(envelope.header.time_quality)) ||
        !writer.u32(processed.config_ref.schema_version) ||
        !writer.u64(processed.config_ref.config_version) ||
        !writer.u8(processed.config_ref.present ? 1U : 0U) ||
        !writer.bytes(processed.config_ref.hash.data(), processed.config_ref.hash.size()) ||
        !writer.u16(static_cast<uint16_t>(processed.profile_refs.size())) ||
        !write_profile_refs(processed.profile_refs, writer) ||
        !writer.u16(static_cast<uint16_t>(processed.source_sample_ids.size()))) return false;
    for (std::size_t index = 0U; index < processed.source_sample_ids.size(); ++index) {
      const auto* source = processed.source_sample_ids.at(index);
      if (source == nullptr || !writer.bytes(source->device_id.bytes.data(), source->device_id.bytes.size()) ||
          !writer.bytes(source->boot_id.bytes.data(), source->boot_id.bytes.size()) ||
          !writer.u64(source->sequence)) return false;
    }
    const auto& values = processed.processed;
    if (!write_metric(values.fill_pct, writer) ||
        !write_metric(values.weight_kg, writer) ||
        !write_metric(values.weight_utilization_pct, writer) ||
        !write_metric(values.ogi, writer) ||
        !write_metric(values.accumulation_hours, writer) ||
        !write_metric(values.temperature_c, writer) ||
        !write_metric(values.mq7_mv, writer) ||
        !write_metric(values.mq8_mv, writer)) return false;
    const auto& fis = envelope.result.fuzzy.value();
    const auto& safety = envelope.result.safety.value();
    return writer.u8(static_cast<uint8_t>(fis.state)) &&
           writer.u8(static_cast<uint8_t>(fis.reason)) &&
           writer.u8(static_cast<uint8_t>(safety.state)) &&
           writer.u8(static_cast<uint8_t>(safety.reason)) &&
           storage::RecordCodec::finish(output, writer);
  }

  static bool write_profile_refs(
      const processing::ProfileRefSet& refs,
      storage::PayloadWriter& writer) noexcept {
    for (std::size_t index = 0U; index < refs.size(); ++index) {
      const auto* ref = refs.at(index);
      if (ref == nullptr || !writer.u8(static_cast<uint8_t>(ref->kind)) ||
          !writer.u32(ref->schema_version) ||
          !writer.u32(ref->profile_version) || !writer.u32(ref->target_id) ||
          !writer.bytes(ref->hash.data(), ref->hash.size())) {
        return false;
      }
    }
    return true;
  }

  static bool write_metric(const processing::ProcessedMetric& metric,
                           storage::PayloadWriter& writer) noexcept {
    return writer.u8(metric.available ? 1U : 0U) &&
           writer.f32(metric.value) &&
           writer.u8(static_cast<uint8_t>(metric.disposition.quality)) &&
           writer.u8(static_cast<uint8_t>(metric.disposition.reason));
  }

  storage::IArchiveStore* store_{nullptr};
  acquisition::ISampleObserver* secondary_{nullptr};
  storage::GapTracker<32U> gaps_{};
  uint64_t durable_records_{0U};
  uint64_t encode_failures_{0U};
};

}  // namespace sigap
