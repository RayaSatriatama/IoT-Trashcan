#pragma once
#include "batch_store.h"
#include "ack_journal_store.h"
#include "runtime.h"
#ifdef ARDUINO
namespace sigap::telemetry_archive {
// Opt-in single-flight generation. Original archive/batch/journal paths are
// never used here. All source reclaim remains in the validated legacy runtime.
class LiveBatchRuntime final {
 public:
  void bind(storage::IArchiveStore* archive, FsImmutableBatchStore* batches,
            FsAckJournalStore* journal) noexcept {
    archive_ = archive; batches_ = batches; journal_ = journal;
    runtime_.bind(archive, batches, journal);
  }
  void bind_debug(debug::IEventSink* sink,
                  acquisition::InjectedMonotonicClock clock) noexcept {
    runtime_.bind_debug(sink, clock);
  }
  BatchRuntimeStatus poll() noexcept {
    if (recovery_blocked_) return BatchRuntimeStatus::Retryable;
    if (!repair_if_needed()) return BatchRuntimeStatus::Retryable;
    if (cleanup_pending_ || runtime_.pending()) return BatchRuntimeStatus::Durable;
    const auto status = runtime_.poll();
    repair_needed_ = status == BatchRuntimeStatus::Retryable;
    return status;
  }
  AckPersistStatus apply_ack(const Ack& ack) noexcept {
    if (recovery_blocked_) return AckPersistStatus::Retryable;
    if (!repair_if_needed()) return AckPersistStatus::Retryable;
    if (cleanup_pending_) {
      if (!ack_matches(runtime_.batch(), ack)) return AckPersistStatus::Invalid;
      return finish_cleanup() ? AckPersistStatus::Durable : AckPersistStatus::Retryable;
    }
    const auto status = runtime_.apply_ack(ack);
    repair_needed_ = status == AckPersistStatus::Retryable;
    if (status != AckPersistStatus::Durable) return status;
    cleanup_ack_ = ack;
    cleanup_pending_ = true;
    return finish_cleanup() ? AckPersistStatus::Durable : AckPersistStatus::Retryable;
  }
  bool pending() const noexcept { return cleanup_pending_ || runtime_.pending(); }
  const ImmutableBatch& batch() const noexcept { return runtime_.batch(); }

  // Replay only a persisted, exact completion proof before new batch admission.
  // Workspace is owned by the application, never a 32 KiB stack temporary.
  bool recover_completed(ImmutableBatch& workspace) noexcept {
    recovery_blocked_ = false;
    const bool ready = recover_impl(workspace);
    recovery_blocked_ = !ready;
    return ready;
  }
 private:
  bool repair_if_needed() noexcept {
    if (!repair_needed_) return true;
    // A partial append is quarantined before any retry, never hidden under a
    // later valid frame. Rebuild indexes without changing borrowed batch bytes.
    if (batches_ == nullptr || journal_ == nullptr ||
        !batches_->recover() || !journal_->recover()) return false;
    repair_needed_ = false;
    return true;
  }
  bool recover_impl(ImmutableBatch& workspace) noexcept {
    if (archive_ == nullptr || batches_ == nullptr || journal_ == nullptr) return false;
    if (batches_->pending_bytes() == 0U) return journal_->empty() && journal_->clear_completed();
    if (!batches_->load(0U, workspace) || !workspace.source.present) return false;
    Ack ack{};
    ack.device_id = workspace.device_id; ack.batch_id_size = workspace.batch_id_size;
    std::copy(workspace.batch_id.begin(), workspace.batch_id.begin() + workspace.batch_id_size,
              ack.batch_id.begin());
    ack.body_hash = workspace.body_hash; ack.record_count = workspace.record_count;
    ack.stored_count = workspace.record_count; ack.status = AckStatus::Durable;
    const storage::ArchiveIdentity source{
        workspace.source.device_id, workspace.source.boot_id, workspace.source.sequence,
        static_cast<storage::ArchiveRecordKind>(workspace.source.record_kind)};
    const auto proof = journal_->completion_proof(ack);
    if (proof == 0U) return archive_->contains(source, workspace.source.record_hash);
    if (proof == 1U) return apply_ack(ack) == AckPersistStatus::Durable;
    if (archive_->contains(source, workspace.source.record_hash)) return false;
    cleanup_ack_ = ack;
    cleanup_pending_ = true;
    return finish_cleanup();
  }
  bool finish_cleanup() noexcept {
    if (batches_ == nullptr || journal_ == nullptr || !journal_->empty()) return false;
    // Journal proof stays durable until the acknowledged body is retired.
    // Keep an empty active file so forensic tails can never be resurrected.
    if (!batches_->retire_completed(cleanup_ack_) || !journal_->clear_completed()) {
      repair_needed_ = true;
      return false;
    }
    cleanup_pending_ = false;
    return true;
  }
  ArchiveBatchRuntime<FsImmutableBatchStore, FsAckJournalStore> runtime_{};
  storage::IArchiveStore* archive_{nullptr};
  FsImmutableBatchStore* batches_{nullptr};
  FsAckJournalStore* journal_{nullptr};
  Ack cleanup_ack_{};
  bool cleanup_pending_{false};
  bool recovery_blocked_{false};
  bool repair_needed_{false};
};
} // namespace sigap::telemetry_archive
#endif
