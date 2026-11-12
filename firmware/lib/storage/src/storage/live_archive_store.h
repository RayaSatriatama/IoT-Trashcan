#pragma once
#include <limits>
#include "legacy_pending_store.h"
namespace sigap::storage {
// New v3 delivery generation. The original archive and its sidecars are never
// opened for writing, renamed, acknowledged, or deleted by this adapter.
class LiveArchiveStore final : public LegacyPendingArchiveStore {
 public:
  static constexpr uint64_t kWorkspaceReserveBytes{64U * 1024U};
  LiveArchiveStore() noexcept
      : LegacyPendingArchiveStore("/sigap/live-v3.bin", "/sigap/live-v3.ckp",
                                  "/sigap/live-v3.gaps") {}
  void set_capacity_probe(CapacityProbe probe, void* context) noexcept {
    live_probe_ = probe; live_context_ = context;
    LittleFsArchiveStore::set_capacity_probe(probe, context);
  }
#ifdef ARDUINO
  bool begin(fs::FS& filesystem, uint64_t active_quota_bytes) noexcept {
    retained_legacy_bytes_ = 0U;
    if (filesystem.exists("/sigap/archive.bin")) {
      File legacy = filesystem.open("/sigap/archive.bin", FILE_READ);
      if (!legacy || legacy.isDirectory()) { if (legacy) legacy.close(); return false; }
      retained_legacy_bytes_ = legacy.size();
      legacy.close();
    }
    return LittleFsArchiveStore::begin(filesystem, active_quota_bytes);
  }
#endif
  uint64_t retained_legacy_bytes() const noexcept { return retained_legacy_bytes_; }
  ArchiveHealth health() const noexcept override {
    const auto status = LegacyPendingArchiveStore::health();
    return status == ArchiveHealth::Healthy && retained_legacy_bytes_ != 0U
               ? ArchiveHealth::Degraded : status;
  }
  AppendStatus append(const ImmutableRecordBytes& record) noexcept override {
#ifdef ARDUINO
    if (!RecordCodec::valid(record)) return LittleFsArchiveStore::append(record);
    // Exact retries must stay durable even if unrelated files consume space.
    if (LittleFsArchiveStore::contains(record.identity, record.hash)) return AppendStatus::Durable;
    uint64_t free_bytes{0U};
    if (live_probe_ == nullptr || !live_probe_(free_bytes, live_context_)) return AppendStatus::Retryable;
    const uint64_t bytes = record.size;
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    if (bytes > (max - kWorkspaceReserveBytes) / 2U ||
        pending_bytes_ > max - kWorkspaceReserveBytes - 2U * bytes ||
        free_bytes < pending_bytes_ + 2U * bytes + kWorkspaceReserveBytes) {
      mark_capacity_blocked();
      return AppendStatus::LoggingBlocked;
    }
#endif
    return LittleFsArchiveStore::append(record);
  }
 private:
  CapacityProbe live_probe_{nullptr};
  void* live_context_{nullptr};
  uint64_t retained_legacy_bytes_{0U};
};
} // namespace sigap::storage
