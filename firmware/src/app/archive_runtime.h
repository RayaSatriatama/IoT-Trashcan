#pragma once

#include <cstddef>
#include <cstdint>

#include "acquisition/handoff.h"
#include "acquisition/clock.h"
#include "debug/types.h"
#include "acquisition/profile_manager.h"
#include "acquisition/record.h"
#include "processing_runtime.h"
#include "storage/archive_store.h"
#include "storage/media_migration.h"
#include "storage/storage_worker.h"
#include "telemetry_archive/batch_store.h"

namespace sigap {

class ArchiveRuntime final {
 public:
  static constexpr std::size_t kDrainBudget{4U};
  using ResultEnvelope = ProcessingRuntime::ResultEnvelope;
  using ResultHandoff = ProcessingRuntime::Handoff;
  using SampleHandoff = acquisition::SampleRing<16U>;

  explicit ArchiveRuntime(storage::IArchiveStore* store = nullptr) noexcept
      : store_(store), worker_(store) {}

  void bind_debug(debug::IEventSink* sink,
                  acquisition::InjectedMonotonicClock clock) noexcept {
    debug_sink_ = sink;
    debug_clock_ = clock;
    emit_storage_state();
    emit_storage_queue();
  }

  void set_store(storage::IArchiveStore* store) noexcept {
    store_ = store;
    worker_.set_store(store);
    emit_storage_state();
  }
  void set_media_migration(storage::ArchiveMediaMigration* migration) noexcept {
    migration_ = migration;
  }
  void set_sequence_allocator(acquisition::SequenceAllocator* sequence) noexcept {
    sequence_ = sequence;
  }
  void set_event_context(const acquisition::DeviceId& device,
                         const acquisition::BootId& boot,
                         const acquisition::ConfigRef& config) noexcept {
    event_device_ = device;
    event_boot_ = boot;
    event_config_ = config;
  }
  storage::MigrationStatus poll_media_migration() noexcept {
    return migration_ == nullptr ? storage::MigrationStatus::Idle
                                  : migration_->migrate_one();
  }
  storage::AppendStatus append_sample(
      const acquisition::SampleRecord& sample) noexcept {
    if (audit_encoded_) return storage::AppendStatus::Retryable;
    if (!storage::RecordCodec::encode_sample(sample, scratch_)) {
      ++encode_failures_;
      gaps_.record(sample.header.sequence, storage::GapReason::InvalidRecord);
      return storage::AppendStatus::Invalid;
    }
    return append(scratch_);
  }

  bool drain_samples(SampleHandoff& handoff) noexcept {
    if (audit_encoded_) return false;
    bool progressed{false};
    acquisition::SampleRecord sample{};
    if (const auto* dropped = handoff.first_dropped(); dropped != nullptr) {
      const auto* last_dropped = handoff.last_dropped();
      record_gap_loss(*dropped,
                      last_dropped == nullptr ? *dropped : *last_dropped,
                      handoff.dropped_pending_count(),
                      storage::GapReason::QueueFull);
      handoff.clear_last_dropped();
      progressed = true;
    }
    std::size_t drained{0U};
    while (drained < kDrainBudget &&
           handoff.try_peek(sample) == acquisition::HandoffStatus::Dequeued) {
      const auto status = append_sample(sample);
      if (status == storage::AppendStatus::Durable ||
          status == storage::AppendStatus::Queued ||
          status == storage::AppendStatus::Invalid) {
        handoff.discard_front();
        progressed = true;
        ++drained;
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

  storage::AppendStatus append_audit_event(
      const acquisition::AuditEvent& event) noexcept {
    if (audit_encoded_) return storage::AppendStatus::Retryable;
    if (sequence_ == nullptr || !event_device_.present || !event_boot_.present) {
      return storage::AppendStatus::Retryable;
    }
    uint64_t sequence{0U};
    if (!sequence_->allocate(sequence)) return storage::AppendStatus::Invalid;
    const storage::ArchiveIdentity identity{
        event_device_, event_boot_, sequence, storage::ArchiveRecordKind::Event};
    storage::PayloadWriter writer{nullptr, 0U};
    if (!storage::RecordCodec::begin(identity, scratch_, writer)) {
      return storage::AppendStatus::Invalid;
    }
    writer = storage::PayloadWriter(
        scratch_.bytes.data() + storage::kArchiveHeaderBytes,
        scratch_.bytes.size() - storage::kArchiveHeaderBytes -
            storage::kArchiveTrailerBytes);
    const auto& config = event.config_ref.present ? event.config_ref : event_config_;
    if (!writer.u8(static_cast<uint8_t>(event.kind)) ||
        !writer.u64(event.monotonic_us) ||
        !writer.u8(config.present ? 1U : 0U) ||
        !writer.u32(config.schema_version) ||
        !writer.u64(config.config_version) ||
        !writer.bytes(config.hash.data(), config.hash.size()) ||
        !writer.u8(event.pending_activation ? 1U : 0U) ||
        !write_profile_ref(event.previous, writer) ||
        !write_profile_ref(event.current, writer) ||
        !storage::RecordCodec::finish(scratch_, writer)) {
      return storage::AppendStatus::Invalid;
    }
    return append(scratch_);
  }

  storage::AppendStatus append_boot_metadata(
      uint64_t monotonic_us, std::string_view firmware_version,
      uint8_t reset_reason) noexcept {
    if (audit_encoded_) return storage::AppendStatus::Retryable;
    if (sequence_ == nullptr || !event_device_.present || !event_boot_.present ||
        !event_config_.present || firmware_version.empty() ||
        firmware_version.size() >= 64U) {
      return storage::AppendStatus::Invalid;
    }
    uint64_t sequence{0U};
    if (!sequence_->allocate(sequence)) return storage::AppendStatus::Invalid;
    const storage::ArchiveIdentity identity{
        event_device_, event_boot_, sequence, storage::ArchiveRecordKind::Metadata};
    storage::PayloadWriter writer{nullptr, 0U};
    if (!storage::RecordCodec::begin(identity, scratch_, writer)) {
      return storage::AppendStatus::Invalid;
    }
    writer = storage::PayloadWriter(
        scratch_.bytes.data() + storage::kArchiveHeaderBytes,
        scratch_.bytes.size() - storage::kArchiveHeaderBytes -
            storage::kArchiveTrailerBytes);
    if (!writer.u8(static_cast<uint8_t>(storage::ArchiveMetadataType::Boot)) ||
        !writer.u64(monotonic_us) || !writer.u8(0U) || !writer.i64(0) ||
        !writer.u8(static_cast<uint8_t>(acquisition::TimeQuality::Unsynced)) ||
        !writer.u32(event_config_.schema_version) ||
        !writer.u64(event_config_.config_version) ||
        !writer.u8(event_config_.present ? 1U : 0U) ||
        !writer.bytes(event_config_.hash.data(), event_config_.hash.size()) ||
        !writer.u16(0U) || !writer.u8(static_cast<uint8_t>(firmware_version.size())) ||
        !writer.bytes(reinterpret_cast<const uint8_t*>(firmware_version.data()),
                      firmware_version.size()) ||
        !writer.u8(reset_reason) || !storage::RecordCodec::finish(scratch_, writer)) {
      return storage::AppendStatus::Invalid;
    }
    return append(scratch_);
  }

  bool drain_audit_events(
      acquisition::ProfileAuditRing<16U>& audit_ring) noexcept {
    if (!audit_encoded_) {
      acquisition::AuditEvent event{};
      if (!audit_ring.try_peek(event)) return false;
      // Keep scratch reserved as soon as the audit is encoded, including
      // retries while another producer owns the worker slot. A queued audit
      // releases scratch only after durable or terminal worker completion.
      if (!encode_audit_event(event, scratch_)) {
        (void)audit_ring.discard_front();
        record_gap(0U, storage::GapReason::InvalidRecord);
        return true;
      }
      audit_identity_ = scratch_.identity;
      audit_encoded_ = true;
    }

    const auto status = append(scratch_);
    if (status == storage::AppendStatus::Durable ||
        status == storage::AppendStatus::Queued) {
      if (status == storage::AppendStatus::Queued) {
        audit_ring_ = &audit_ring;
        audit_queued_ = true;
        return true;
      }
      (void)audit_ring.discard_front();
      audit_encoded_ = false;
      return true;
    }
    if (status == storage::AppendStatus::Invalid) {
      (void)audit_ring.discard_front();
      audit_encoded_ = false;
      record_gap(audit_identity_.sequence,
                 storage::GapReason::InvalidRecord);
      return true;
    }
    return false;
  }

  bool drain_results(ResultHandoff& handoff) noexcept {
    if (audit_encoded_) return false;
    bool progressed{false};
    ResultEnvelope envelope{};
    std::size_t drained{0U};
    while (drained < kDrainBudget &&
           handoff.try_peek(envelope) == processing::ResultHandoffStatus::Dequeued) {
      if (!encode_result(envelope, scratch_)) {
        ++encode_failures_;
        handoff.discard_front();
        record_gap(envelope.header.sequence, storage::GapReason::InvalidRecord);
        progressed = true;
        ++drained;
        continue;
      }
      const auto status = append(scratch_);
      if (status != storage::AppendStatus::Durable &&
          status != storage::AppendStatus::Queued) break;
      handoff.discard_front();
      progressed = true;
      ++drained;
    }
    return progressed;
  }

  uint64_t durable_records() const noexcept { return durable_records_; }
  uint64_t encode_failures() const noexcept { return encode_failures_; }
  uint64_t gap_persistence_failures() const noexcept {
    return gap_persistence_failures_;
  }
  uint64_t gap_persisted_ranges() const noexcept {
    return gap_persisted_ranges_;
  }
  const storage::GapTracker<32U>& gaps() const noexcept { return gaps_; }
  storage::ArchiveHealth health() const noexcept {
    return store_ == nullptr ? storage::ArchiveHealth::Degraded : store_->health();
  }
  const storage::StorageWorker<1U>& storage_worker() const noexcept {
    return worker_;
  }
  storage::AppendStatus poll_storage() noexcept {
    if (worker_.queue().size() == 0U) return storage::AppendStatus::Durable;
    const auto* pending = worker_.queue().peek();
    const uint64_t sequence = pending == nullptr ? 0U : pending->identity.sequence;
    const auto pending_identity = pending == nullptr
                                      ? storage::ArchiveIdentity{}
                                      : pending->identity;
    const uint64_t durable_before = worker_.stats().durable;
    const uint64_t terminal_before = worker_.stats().terminal;
    (void)worker_.process_one();
    if (worker_.stats().durable != durable_before) {
      ++durable_records_;
      emit_storage_outcome(0);
      emit_storage_queue();
      emit_storage_state();
      if (audit_queued_ && audit_ring_ != nullptr &&
          same_identity(pending_identity, audit_identity_)) {
        (void)audit_ring_->discard_front();
        audit_ring_ = nullptr;
        audit_queued_ = false;
        audit_encoded_ = false;
      }
      return storage::AppendStatus::Durable;
    }
    if (worker_.stats().terminal != terminal_before) {
      record_gap(sequence, storage::GapReason::InvalidRecord);
      emit_storage_outcome(3);
      emit_storage_queue();
      emit_storage_state();
      if (audit_queued_ && audit_ring_ != nullptr &&
          same_identity(pending_identity, audit_identity_)) {
        (void)audit_ring_->discard_front();
        audit_ring_ = nullptr;
        audit_queued_ = false;
        audit_encoded_ = false;
      }
      return storage::AppendStatus::Invalid;
    }
    emit_storage_state();
    return storage::AppendStatus::Retryable;
  }

  // Run a bounded storage slice outside acquisition and processing callbacks.
  // The one-record poll above remains the completion-aware path for audit
  // events; this helper admits a larger byte budget without moving filesystem
  // ownership into producer code.
  std::size_t poll_storage_slice(
      std::size_t record_budget,
      std::size_t byte_budget = 32U * 1024U) noexcept {
    std::size_t completed{0U};
    std::size_t attempted_bytes{0U};
    while (completed < record_budget && worker_.queue().size() != 0U) {
      const auto* pending = worker_.queue().peek();
      if (pending == nullptr || pending->record == nullptr) break;
      const std::size_t record_bytes = pending->record->size;
      if (completed != 0U && attempted_bytes + record_bytes > byte_budget) break;
      const uint64_t before = worker_.stats().durable + worker_.stats().terminal;
      const auto status = poll_storage();
      if (status == storage::AppendStatus::Retryable) break;
      const uint64_t after = worker_.stats().durable + worker_.stats().terminal;
      if (after == before) break;
      attempted_bytes += record_bytes;
      ++completed;
    }
    return completed;
  }

 private:
  void emit_storage_state() noexcept {
    if (debug_sink_ == nullptr) return;
    const auto current = health();
    const int32_t state = store_ == nullptr ? 0 :
        current == storage::ArchiveHealth::Healthy ? 1 :
        current == storage::ArchiveHealth::Degraded ? 2 : 0;
    const int32_t reason = store_ == nullptr ? 2 : 0;
    if (storage_state_seen_ && state == last_storage_state_ &&
        reason == last_storage_reason_) return;
    debug::EventInput event{};
    event.monotonic_us = debug_clock_.now_us();
    event.module = debug::Module::Storage;
    event.event_code = 30U;
    event.arg0 = state;
    event.arg1 = reason;
    if (debug_sink_->emit(event)) {
      storage_state_seen_ = true;
      last_storage_state_ = state;
      last_storage_reason_ = reason;
    }
  }

  void emit_storage_queue() noexcept {
    if (debug_sink_ == nullptr) return;
    const auto depth = static_cast<int32_t>(worker_.queue().size());
    const auto capacity = static_cast<int32_t>(worker_.queue().capacity());
    if (queue_seen_ && depth == last_queue_depth_ &&
        capacity == last_queue_capacity_) return;
    debug::EventInput event{};
    event.monotonic_us = debug_clock_.now_us();
    event.module = debug::Module::Storage;
    event.event_code = 31U;
    event.arg0 = depth;
    event.arg1 = capacity;
    if (debug_sink_->emit(event)) {
      queue_seen_ = true;
      last_queue_depth_ = depth;
      last_queue_capacity_ = capacity;
    }
  }

  void emit_storage_outcome(int32_t result, uint32_t count = 1U) noexcept {
    if (debug_sink_ == nullptr) return;
    debug::EventInput event{};
    event.monotonic_us = debug_clock_.now_us();
    event.module = debug::Module::Storage;
    event.event_code = 32U;
    event.arg0 = result;
    event.arg1 = static_cast<int32_t>(count);
    (void)debug_sink_->emit(event);
  }

  static bool same_identity(const storage::ArchiveIdentity& left,
                            const storage::ArchiveIdentity& right) noexcept {
    return left.kind == right.kind && left.sequence == right.sequence &&
           left.device_id.bytes == right.device_id.bytes &&
           left.boot_id.bytes == right.boot_id.bytes;
  }

  storage::AppendStatus append(const storage::ImmutableRecordBytes& record) noexcept {
    if (store_ == nullptr) {
      return storage::AppendStatus::Retryable;
    }
    // Producers only transfer ownership to the worker. Filesystem I/O runs
    // from poll_storage(), outside acquisition and processing drains.
    if (worker_.queue().size() != 0U) {
      return storage::AppendStatus::Retryable;
    }
    if (!worker_.try_enqueue_copy(record)) return storage::AppendStatus::Retryable;
    emit_storage_queue();
    emit_storage_state();
    return storage::AppendStatus::Queued;
  }

  bool encode_audit_event(const acquisition::AuditEvent& event,
                          storage::ImmutableRecordBytes& output) noexcept {
    if (sequence_ == nullptr || !event_device_.present || !event_boot_.present) {
      return false;
    }
    uint64_t sequence{0U};
    if (!sequence_->allocate(sequence)) return false;
    const storage::ArchiveIdentity identity{
        event_device_, event_boot_, sequence, storage::ArchiveRecordKind::Event};
    storage::PayloadWriter writer{nullptr, 0U};
    if (!storage::RecordCodec::begin(identity, output, writer)) return false;
    writer = storage::PayloadWriter(
        output.bytes.data() + storage::kArchiveHeaderBytes,
        output.bytes.size() - storage::kArchiveHeaderBytes -
            storage::kArchiveTrailerBytes);
    const auto& config = event.config_ref.present ? event.config_ref : event_config_;
    if (!writer.u8(static_cast<uint8_t>(event.kind)) ||
        !writer.u64(event.monotonic_us) ||
        !writer.u8(config.present ? 1U : 0U) ||
        !writer.u32(config.schema_version) ||
        !writer.u64(config.config_version) ||
        !writer.bytes(config.hash.data(), config.hash.size()) ||
        !writer.u8(event.pending_activation ? 1U : 0U) ||
        !write_profile_ref(event.previous, writer) ||
        !write_profile_ref(event.current, writer) ||
        !storage::RecordCodec::finish(output, writer)) {
      return false;
    }
    return true;
  }

  void record_gap(uint64_t sequence, storage::GapReason reason) noexcept {
    if (!gaps_.record(sequence, reason)) {
      ++gap_persistence_failures_;
      return;
    }
    persist_latest_gap();
  }

  void record_gap(const acquisition::SampleRecord& sample,
                  storage::GapReason reason) noexcept {
    if (!gaps_.record(sample, reason)) {
      ++gap_persistence_failures_;
      return;
    }
    persist_latest_gap();
  }

  void record_gap_loss(const acquisition::SampleRecord& first,
                       const acquisition::SampleRecord& last,
                       uint64_t count,
                       storage::GapReason reason) noexcept {
    if (!gaps_.record_loss(first, last, count, reason)) {
      ++gap_persistence_failures_;
      return;
    }
    persist_latest_gap();
  }

  void persist_latest_gap() noexcept {
    if (store_ == nullptr || gaps_.size() == 0U) {
      ++gap_persistence_failures_;
      return;
    }
    const auto status = store_->append_gap(gaps_.at(gaps_.size() - 1U));
    if (status == storage::AppendStatus::Durable) {
      ++gap_persisted_ranges_;
    } else {
      ++gap_persistence_failures_;
    }
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
      if (ref == nullptr || !write_profile_ref(*ref, writer)) {
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
  debug::IEventSink* debug_sink_{nullptr};
  acquisition::InjectedMonotonicClock debug_clock_{nullptr, nullptr};
  bool storage_state_seen_{false};
  int32_t last_storage_state_{-1};
  int32_t last_storage_reason_{-1};
  bool queue_seen_{false};
  int32_t last_queue_depth_{-1};
  int32_t last_queue_capacity_{-1};
  // ArchiveRuntime transfers one immutable frame at a time. Upstream typed
  // handoffs retain additional records until this slot drains, so the target
  // composition does not reserve multiple 16 KiB frame references in DRAM.
  storage::StorageWorker<1U> worker_{};
  storage::ArchiveMediaMigration* migration_{nullptr};
  acquisition::SequenceAllocator* sequence_{nullptr};
  acquisition::DeviceId event_device_{};
  acquisition::BootId event_boot_{};
  acquisition::ConfigRef event_config_{};
  // The cooperative runtime encodes one record at a time. Keeping this
  // bounded buffer in the instance avoids a 16 KiB automatic loop-stack
  // allocation and keeps filesystem work outside acquisition callbacks.
  storage::ImmutableRecordBytes scratch_{};
  storage::ArchiveIdentity audit_identity_{};
  bool audit_encoded_{false};
  acquisition::ProfileAuditRing<16U>* audit_ring_{nullptr};
  bool audit_queued_{false};
  storage::GapTracker<32U> gaps_{};
  uint64_t durable_records_{0U};
  uint64_t encode_failures_{0U};
  uint64_t gap_persistence_failures_{0U};
  uint64_t gap_persisted_ranges_{0U};
};

}  // namespace sigap
