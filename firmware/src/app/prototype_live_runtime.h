#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "acquisition/clock.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"
#include "storage/archive_store.h"

namespace sigap {

// Prototype volatile single-flight batch runtime.
//
// Uses RAM-only stores (MemoryArchiveStore, MemoryImmutableBatchStore,
// MemoryAckJournalStore). It does NOT provide power-loss durability; ACK
// success means protocol-level completion of a volatile batch, not lossless
// delivery and not disk durability. Old FS stores are never read, reclaimed,
// or deleted by this runtime.
//
// Stage 2 behaviour:
// - Bounded multi-sample batching. Consecutive FIFO sample records that share
//   one device identity and one boot identity are composed into one body, up
//   to the frozen contract bounds (128 records / 32 KiB). A record of a
//   different kind, device or boot ends the group; it is never dropped,
//   reordered or allowed to block the head. Metadata, event and result records
//   keep the existing one-record path.
// - The exact body, batch ID, body hash, record count and the full source
//   manifest (identity + archive hash per record) are frozen while HTTP is in
//   flight. Nothing is rebuilt or re-selected before the ACK decision.
// - A whole-batch ACK must match device, batch ID, body hash, record count and
//   stored + deduplicated == record count. Only then are sources reclaimed,
//   head-first in manifest order. Reclaim progress is retained across partial
//   failures and a retry never re-reclaims the retired prefix and never skips
//   an unexpected FIFO head.
// - A frozen source that vanished or changed before the ACK is a fail-closed
//   fault: the ACK is reported non-Durable while the exact body, ID, hash,
//   count, manifest and reclaim cursor are retained unchanged. Nothing is
//   rebuilt, dropped or skipped, and the batch keeps holding the pipeline
//   until an operator recovers it.
class PrototypeLiveBatchRuntime final {
 public:
  // Bounded lookahead/group limits. kMaxGroupRecords bounds only how many
  // records one body selects from the FIFO head; it reserves no store
  // capacity. The 16-slot RAM store may already be full when a group is
  // composed, and selected records stay queued until the ACK reclaims them, so
  // free slots are not guaranteed while a batch is in flight. The immutable
  // batch contract still bounds the body at 128 records / 32 KiB; a record
  // that does not fit ends the group.
  static constexpr std::size_t kMaxGroupRecords{8U};
  static constexpr std::size_t kBatchTargetRecords{6U};
  static constexpr uint64_t kBatchMaxWaitUs{500000U};

  struct ManifestEntry final {
    acquisition::DeviceId device_id{};
    acquisition::BootId boot_id{};
    uint64_t sequence{0U};
    uint8_t record_kind{0U};
    config::ConfigHash record_hash{};
    bool present{false};
  };

  // Binds the RAM archive store (caller owns the store lifetime). Rebinding
  // drops any in-flight batch rather than keeping a manifest that points at a
  // different store. A previously bound clock stays bound.
  void bind(storage::IArchiveStore* archive) noexcept {
    archive_ = archive;
    reset_flight();
    accumulate_started_ = false;
  }

  // Retained for callers that only pass opaque debug handles.
  void bind_debug(const void* sink, const void* clock) noexcept {
    debug_sink_ = sink;
    debug_clock_ = clock;
  }

  // Binds the real monotonic clock used for bounded accumulation. Without a
  // bound clock the runtime keeps the historical immediate-build behaviour so
  // host fixtures and callers that do not own a clock stay deterministic.
  void bind_clock(acquisition::InjectedMonotonicClock clock) noexcept {
    clock_ = clock;
    clock_bound_ = true;
    accumulate_started_ = false;
  }

  // Polls the RAM archive, composes at most one batch and freezes it.
  // Returns Durable when a batch is submitted and pending ACK, Idle when no
  // record (or no accumulated group) is ready, and Retryable/Invalid on error.
  telemetry_archive::BatchRuntimeStatus poll() noexcept {
    if (archive_ == nullptr) return telemetry_archive::BatchRuntimeStatus::Retryable;
    if (pending_) return telemetry_archive::BatchRuntimeStatus::Durable;

    if (!archive_->peek_pending(record_)) {
      accumulate_started_ = false;
      return telemetry_archive::BatchRuntimeStatus::Idle;
    }
    // The head is read in full by peek_pending() on every poll (including the
    // Idle polls inside the accumulation window); the same 16 KiB scratch
    // record is reused for the later lookahead, so the runtime still adds no
    // second record-sized buffer. Only entries at index >= 1 use the
    // identity-only seam.
    head_ = record_.identity;

    if (head_.kind != storage::ArchiveRecordKind::Sample) {
      // Metadata/event/result keep the existing one-record path and are never
      // delayed by sample accumulation.
      if (!builder_.build(record_, batch_)) {
        return telemetry_archive::BatchRuntimeStatus::Invalid;
      }
      manifest_[0] = manifest_entry(record_);
      manifest_count_ = 1U;
      reclaimed_count_ = 0U;
      accumulate_started_ = false;
      pending_ = true;
      ++batches_built_;
      return telemetry_archive::BatchRuntimeStatus::Durable;
    }

    std::size_t available{0U};
    const std::size_t compatible = count_compatible(available);
    if (compatible == 0U) return telemetry_archive::BatchRuntimeStatus::Invalid;

    if (!accumulation_ready(compatible, available)) {
      return telemetry_archive::BatchRuntimeStatus::Idle;
    }

    if (!build_group(compatible)) {
      return telemetry_archive::BatchRuntimeStatus::Invalid;
    }
    accumulate_started_ = false;
    pending_ = true;
    ++batches_built_;
    return telemetry_archive::BatchRuntimeStatus::Durable;
  }

  // Applies an ACK from the network. Requires a pending batch and an exact
  // whole-batch match. Returns Durable once every frozen source is reclaimed,
  // Retryable while reclaim progress is incomplete or a frozen source is
  // missing/changed (the frozen batch is retained), and Invalid when the ACK
  // itself does not match the frozen batch.
  telemetry_archive::AckPersistStatus apply_ack(
      const telemetry_archive::Ack& ack) noexcept {
    if (!pending_ || archive_ == nullptr) {
      return telemetry_archive::AckPersistStatus::Retryable;
    }
    if (!telemetry_archive::ack_matches(batch_, ack) || !batch_.source.present) {
      return telemetry_archive::AckPersistStatus::Invalid;
    }
    if (manifest_count_ == 0U || reclaimed_count_ > manifest_count_ ||
        batch_.record_count != manifest_count_) {
      return telemetry_archive::AckPersistStatus::Invalid;
    }

    // Prevalidate every remaining source before reclaiming anything further. A
    // source that vanished or changed since the body was frozen can never be
    // retired, so the ACK must not be reported Durable. This is fail-closed:
    // the exact body, batch ID, body hash, record count, manifest and reclaim
    // cursor are retained unchanged, nothing is rebuilt, dropped or skipped,
    // and an already-reclaimed prefix stays recorded and is never required
    // again. The batch therefore keeps holding the pipeline until an operator
    // recovers it (restore the source or rebind/restart); there is no implicit
    // abandonment or recovery fallback.
    for (std::size_t index = reclaimed_count_; index < manifest_count_; ++index) {
      if (!manifest_[index].present ||
          !archive_->contains(identity_of(manifest_[index]),
                              manifest_[index].record_hash)) {
        // One batch event per flight: a repeated ACK retry of the same fault
        // must not inflate the counter.
        if (!source_mismatch_charged_) {
          ++source_mismatch_count_;
          source_mismatch_charged_ = true;
        }
        return telemetry_archive::AckPersistStatus::Retryable;
      }
    }

    // Reclaim head-first in manifest order. Each step only removes the exact
    // FIFO head; a mismatch stops the loop and keeps the completed prefix.
    while (reclaimed_count_ < manifest_count_) {
      const ManifestEntry& entry = manifest_[reclaimed_count_];
      if (!archive_->reclaim_pending(identity_of(entry), entry.record_hash)) {
        // Partial means progress was already made on this flight; a total
        // failure with zero reclaimed sources is not a partial reclaim.
        if (reclaimed_count_ > 0U && !partial_reclaim_charged_) {
          ++partial_reclaim_count_;
          partial_reclaim_charged_ = true;
        }
        return telemetry_archive::AckPersistStatus::Retryable;
      }
      ++reclaimed_count_;
    }

    records_delivered_ += manifest_count_;
    ++batches_delivered_;
    reset_flight();
    return telemetry_archive::AckPersistStatus::Durable;
  }

  // True when a batch is in-flight (submitted, awaiting ACK).
  bool pending() const noexcept { return pending_; }

  // Returns the current in-flight batch (valid only when pending()).
  const telemetry_archive::ImmutableBatch& batch() const noexcept { return batch_; }

  // Diagnostics. These distinguish batches from records on purpose: the
  // network "delivered" outcome is one batch, not one record.
  std::size_t manifest_size() const noexcept { return manifest_count_; }
  std::size_t reclaimed_size() const noexcept { return reclaimed_count_; }
  uint64_t batches_built() const noexcept { return batches_built_; }
  uint64_t batches_delivered() const noexcept { return batches_delivered_; }
  uint64_t records_delivered() const noexcept { return records_delivered_; }
  // Per-batch failure events, each charged at most once per flight.
  // source_mismatch_count() counts flights whose frozen manifest was rejected
  // because a source is missing or changed; partial_reclaim_count() counts
  // flights whose reclaim stopped after a completed prefix (progress > 0). A
  // total reclaim failure with no progress is not a partial reclaim, and a
  // repeated retry of the same fault does not increment either counter again.
  uint64_t source_mismatch_count() const noexcept { return source_mismatch_count_; }
  uint64_t partial_reclaim_count() const noexcept { return partial_reclaim_count_; }

 private:
  static storage::ArchiveIdentity identity_of(const ManifestEntry& entry) noexcept {
    return storage::ArchiveIdentity{
        entry.device_id, entry.boot_id, entry.sequence,
        static_cast<storage::ArchiveRecordKind>(entry.record_kind)};
  }

  static ManifestEntry manifest_entry(
      const storage::ImmutableRecordBytes& record) noexcept {
    ManifestEntry entry{};
    entry.device_id = record.identity.device_id;
    entry.boot_id = record.identity.boot_id;
    entry.sequence = record.identity.sequence;
    entry.record_kind = static_cast<uint8_t>(record.identity.kind);
    entry.record_hash = record.hash;
    entry.present = true;
    return entry;
  }

  // Counts the leading FIFO run of sample records that share the head's device
  // and boot identity. `available` reports how many records were inspected.
  // Only the entries at index >= 1 use the cheap identity-only seam; the head
  // itself was already copied in full by poll(). Stores without the seam still
  // report the head, so a single-record batch keeps working on every released
  // store.
  std::size_t count_compatible(std::size_t& available) noexcept {
    available = 1U;
    std::size_t compatible{1U};
    for (std::size_t index = 1U; index < kMaxGroupRecords; ++index) {
      storage::ArchiveIdentity next{};
      if (!archive_->peek_pending_identity_at(index, next)) break;
      ++available;
      if (next.kind != storage::ArchiveRecordKind::Sample) break;
      if (next.device_id.bytes != head_.device_id.bytes ||
          next.boot_id.bytes != head_.boot_id.bytes) {
        break;
      }
      ++compatible;
    }
    return compatible;
  }

  // Bounded accumulation: build as soon as a target group is available, wait a
  // bounded window for more records, and flush whatever is available when the
  // window expires. A different record behind the run never delays the head.
  bool accumulation_ready(std::size_t compatible, std::size_t available) noexcept {
    if (compatible >= kBatchTargetRecords) return true;
    if (available > compatible) return true;  // a different record is behind it
    if (!clock_bound_) return true;
    const uint64_t now = clock_.now_us();
    if (!accumulate_started_ ||
        accumulate_head_sequence_ != head_.sequence ||
        accumulate_head_kind_ != static_cast<uint8_t>(head_.kind)) {
      accumulate_started_ = true;
      accumulate_started_us_ = now;
      accumulate_head_sequence_ = head_.sequence;
      accumulate_head_kind_ = static_cast<uint8_t>(head_.kind);
      return false;
    }
    return now >= accumulate_started_us_ &&
           now - accumulate_started_us_ >= kBatchMaxWaitUs;
  }

  bool build_group(std::size_t compatible) noexcept {
    telemetry_archive::BatchJsonBuilder::SampleGroupBuilder group{};
    if (!group.begin(batch_, head_, static_cast<uint16_t>(kMaxGroupRecords))) {
      return false;
    }
    manifest_count_ = 0U;
    reclaimed_count_ = 0U;
    for (std::size_t index = 0U; index < compatible; ++index) {
      // record_ still holds the FIFO head from poll(); only later records need a
      // byte copy, and nothing is reclaimed here.
      if (index != 0U && !archive_->peek_pending_at(index, record_)) break;
      const auto status = group.add(record_);
      if (status == telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::
                        AddStatus::Added) {
        manifest_[manifest_count_++] = manifest_entry(record_);
        continue;
      }
      // Full or an incompatible record: stop here and let the next poll start
      // a new batch from that exact record.
      break;
    }
    if (manifest_count_ == 0U) return false;
    if (!group.finish(batch_)) return false;
    if (batch_.record_count != manifest_count_) return false;
    return true;
  }

  void reset_flight() noexcept {
    pending_ = false;
    manifest_count_ = 0U;
    reclaimed_count_ = 0U;
    source_mismatch_charged_ = false;
    partial_reclaim_charged_ = false;
    record_.size = 0U;
    batch_.body_size = 0U;
    batch_.record_count = 0U;
    batch_.attempted = false;
    batch_.source.present = false;
    batch_.batch_id_size = 0U;
  }

  storage::IArchiveStore* archive_{nullptr};
  const void* debug_sink_{nullptr};
  const void* debug_clock_{nullptr};

  telemetry_archive::BatchJsonBuilder builder_{};
  storage::ImmutableRecordBytes record_{};
  storage::ArchiveIdentity head_{};
  telemetry_archive::ImmutableBatch batch_{};
  std::array<ManifestEntry, kMaxGroupRecords> manifest_{};
  std::size_t manifest_count_{0U};
  std::size_t reclaimed_count_{0U};
  bool pending_{false};

  acquisition::InjectedMonotonicClock clock_{nullptr, nullptr};
  bool clock_bound_{false};
  bool accumulate_started_{false};
  uint64_t accumulate_started_us_{0U};
  uint64_t accumulate_head_sequence_{0U};
  uint8_t accumulate_head_kind_{0U};

  uint64_t batches_built_{0U};
  uint64_t batches_delivered_{0U};
  uint64_t records_delivered_{0U};
  uint64_t source_mismatch_count_{0U};
  uint64_t partial_reclaim_count_{0U};
  // Per-flight latches so a repeated ACK retry counts the same batch event
  // once. Both reset with the flight.
  bool source_mismatch_charged_{false};
  bool partial_reclaim_charged_{false};
};

// The runtime owns one 32 KiB batch body and one 16 KiB scratch record. Keep
// the instance bounded so the prototype path cannot silently grow the static
// RAM footprint of SigapApp.
static_assert(sizeof(PrototypeLiveBatchRuntime) <= 56U * 1024U,
              "prototype batch runtime must stay within its bounded RAM budget");

}  // namespace sigap
