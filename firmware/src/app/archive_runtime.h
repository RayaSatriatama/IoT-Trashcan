#pragma once

#include <cstddef>
#include <cstdint>

#include "acquisition/handoff.h"
#include "acquisition/record.h"
#include "processing_runtime.h"
#include "storage/archive_store.h"
#include "telemetry_archive/batch_store.h"

namespace sigap {

class ArchiveRuntime final {
 public:
  using ResultEnvelope = ProcessingRuntime::ResultEnvelope;
  using ResultHandoff = ProcessingRuntime::Handoff;
  using SampleHandoff = acquisition::SampleRing<16U>;

  explicit ArchiveRuntime(storage::IArchiveStore* store = nullptr) noexcept
      : store_(store) {}

  void set_store(storage::IArchiveStore* store) noexcept {
    store_ = store;
  }
  storage::AppendStatus append_sample(
      const acquisition::SampleRecord& sample) noexcept {
    if (!storage::RecordCodec::encode_sample(sample, scratch_)) {
      ++encode_failures_;
      gaps_.record(sample.header.sequence, storage::GapReason::InvalidRecord);
      return storage::AppendStatus::Invalid;
    }
    return append(scratch_);
  }

  bool drain_samples(SampleHandoff& handoff) noexcept {
    bool progressed{false};
    acquisition::SampleRecord sample{};
    if (const auto* dropped = handoff.first_dropped(); dropped != nullptr) {
      gaps_.record(*dropped, storage::GapReason::QueueFull);
      const auto* last_dropped = handoff.last_dropped();
      if (last_dropped != nullptr &&
          last_dropped->header.sequence != dropped->header.sequence) {
        gaps_.record(*last_dropped, storage::GapReason::QueueFull);
      }
      handoff.clear_last_dropped();
      progressed = true;
    }
    while (handoff.try_peek(sample) == acquisition::HandoffStatus::Dequeued) {
      const auto status = append_sample(sample);
      if (status == storage::AppendStatus::Durable ||
          status == storage::AppendStatus::Invalid) {
        handoff.discard_front();
        progressed = true;
        continue;
      }
      break;
    }
    return progressed;
  }

  storage::AppendStatus append_record(
      const storage::ImmutableRecordBytes& record) noexcept {
    return append(record);
  }

  bool drain_results(ResultHandoff& handoff) noexcept {
    bool progressed{false};
    ResultEnvelope envelope{};
    while (handoff.try_peek(envelope) == processing::ResultHandoffStatus::Dequeued) {
      if (!encode_result(envelope, scratch_)) {
        ++encode_failures_;
        handoff.discard_front();
        gaps_.record(envelope.header.sequence, storage::GapReason::InvalidRecord);
        progressed = true;
        continue;
      }
      if (append(scratch_) != storage::AppendStatus::Durable) break;
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
    else if (status == storage::AppendStatus::Invalid) {
      gaps_.record(record.identity.sequence, storage::GapReason::InvalidRecord);
    }
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
    return write_fis(fis, writer) && write_safety(safety, writer) &&
           storage::RecordCodec::finish(output, writer);
  }

  static bool write_model_id(const models::ModelId& id,
                             storage::PayloadWriter& writer) noexcept {
    return writer.bytes(reinterpret_cast<const uint8_t*>(id.data()), id.size());
  }

  static bool write_profile_ref(const acquisition::ProfileRef& ref,
                                storage::PayloadWriter& writer) noexcept {
    return writer.u8(ref.present ? 1U : 0U) &&
           writer.u8(static_cast<uint8_t>(ref.kind)) &&
           writer.u32(ref.schema_version) && writer.u32(ref.profile_version) &&
           writer.u32(ref.target_id) && writer.bytes(ref.hash.data(), ref.hash.size());
  }

  static bool write_fis(const fuzzy::FisResult& fis,
                        storage::PayloadWriter& writer) noexcept {
    if (fis.trace.membership_count > fis.trace.memberships.size() ||
        fis.trace.rule_count > fis.trace.rules.size()) return false;
    if (!writer.u8(static_cast<uint8_t>(fis.state)) ||
        !writer.u8(static_cast<uint8_t>(fis.reason)) ||
        !writer.u8(static_cast<uint8_t>(fis.model_state)) ||
        !writer.u8(fis.score_present ? 1U : 0U) || !writer.f32(fis.score) ||
        !writer.u8(fis.label_present ? 1U : 0U) || !write_model_id(fis.label, writer) ||
        !writer.u8(fis.trace_present ? 1U : 0U) ||
        !writer.u32(fis.trace.model_ref.version) ||
        !writer.bytes(fis.trace.model_ref.hash.data(), fis.trace.model_ref.hash.size()) ||
        !writer.u16(static_cast<uint16_t>(fis.trace.membership_count))) return false;
    for (std::size_t i = 0U; i < fis.trace.membership_count; ++i) {
      const auto& membership = fis.trace.memberships[i];
      if (!writer.u8(static_cast<uint8_t>(membership.input)) ||
          !write_model_id(membership.set_id, writer) || !writer.f32(membership.value)) {
        return false;
      }
    }
    if (!writer.u16(static_cast<uint16_t>(fis.trace.rule_count))) return false;
    for (std::size_t i = 0U; i < fis.trace.rule_count; ++i) {
      const auto& rule = fis.trace.rules[i];
      if (!writer.u16(rule.id) || !writer.f32(rule.strength) ||
          !write_model_id(rule.output_id, writer)) return false;
    }
    return writer.u8(fis.trace.aggregate.present ? 1U : 0U) &&
           write_model_id(fis.trace.aggregate.output_id, writer) &&
           writer.f32(fis.trace.aggregate.area) &&
           writer.u8(fis.trace.aggregate.centroid_present ? 1U : 0U) &&
           writer.f32(fis.trace.aggregate.centroid);
  }

  static bool write_safety(const safety::SafetyResult& safety_result,
                           storage::PayloadWriter& writer) noexcept {
    const auto& trace = safety_result.trace;
    if (trace.evidence_count > trace.evidence.size()) return false;
    if (!writer.u8(static_cast<uint8_t>(safety_result.state)) ||
        !writer.u8(static_cast<uint8_t>(safety_result.reason)) ||
        !writer.u8(static_cast<uint8_t>(safety_result.model_state)) ||
        !write_profile_ref(trace.model_ref, writer) ||
        !writer.u8(trace.evidence_count) || !writer.u8(trace.clear_streak) ||
        !writer.u8(trace.alert_latched_before ? 1U : 0U) ||
        !writer.u8(trace.alert_latched_after ? 1U : 0U)) return false;
    for (std::size_t i = 0U; i < trace.evidence_count; ++i) {
      const auto& evidence = trace.evidence[i];
      if (!writer.u8(static_cast<uint8_t>(evidence.signal)) ||
          !writer.u8(static_cast<uint8_t>(evidence.input_state)) ||
          !writer.f32(evidence.value) || !writer.f32(evidence.alert_at_or_above) ||
          !writer.f32(evidence.clear_below) ||
          !writer.u8(evidence.threshold_exceeded ? 1U : 0U) ||
          !writer.u8(evidence.clear_condition_met ? 1U : 0U)) return false;
    }
    return true;
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
  // The cooperative runtime encodes one record at a time. Keeping this
  // bounded buffer in the instance avoids a 16 KiB automatic loop-stack
  // allocation and keeps filesystem work outside acquisition callbacks.
  storage::ImmutableRecordBytes scratch_{};
  storage::GapTracker<32U> gaps_{};
  uint64_t durable_records_{0U};
  uint64_t encode_failures_{0U};
};

}  // namespace sigap
