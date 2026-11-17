#pragma once
#include <limits>
#include <cstring>
#include "legacy_pending_store.h"
#include "live_delivery_paths.h"
namespace sigap::storage {
// New v3 delivery generation. The original archive and its sidecars are never
// opened for writing, renamed, acknowledged, or deleted by this adapter.
class LiveArchiveStore final : public LegacyPendingArchiveStore {
 public:
  static constexpr uint64_t kWorkspaceReserveBytes{64U * 1024U};
  LiveArchiveStore() noexcept
      : LegacyPendingArchiveStore(live_delivery::kArchive,
                                  live_delivery::kCheckpoint,
                                  live_delivery::kGaps) {}
  LiveArchiveStore(const char* archive, const char* checkpoint,
                   const char* gaps) noexcept
      : LegacyPendingArchiveStore(archive, checkpoint, gaps) {}
  void set_capacity_probe(CapacityProbe probe, void* context) noexcept {
    live_probe_ = probe; live_context_ = context;
    LittleFsArchiveStore::set_capacity_probe(probe, context);
  }
#ifdef ARDUINO
  bool begin(fs::FS& filesystem, uint64_t active_quota_bytes) noexcept {
    retained_legacy_bytes_ = 0U;
    if (!account_read_only_file(filesystem, "/sigap/archive.bin")) return false;
    if (live_delivery::kPriorityFresh) {
      constexpr const char* old_live_files[]{
          "/sigap/live-v3.bin", "/sigap/live-v3.ckp", "/sigap/live-v3.gaps",
          "/sigap/live-v3.batches", "/sigap/live-v3.batches.ckp",
          "/sigap/live-v3.acks", "/sigap/live-v3.acks.ckp",
          "/sigap/web-live-v3.bin", "/sigap/web-live-v3.ckp", "/sigap/web-live-v3.gaps",
          "/sigap/web-live-v3.batches", "/sigap/web-live-v3.batches.ckp",
          "/sigap/web-live-v3.acks", "/sigap/web-live-v3.acks.ckp"};
      for (const char* path : old_live_files) {
        if (!account_read_only_file(filesystem, path)) return false;
      }
      if (!account_previous_web_boots(filesystem)) return false;
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
#ifdef ARDUINO
  bool account_previous_web_boots(fs::FS& filesystem) noexcept {
    char current[64]{};
    const char* archive = path_;
    if (archive == nullptr) return false;
    const char* suffix = std::strrchr(archive, '.');
    if (suffix == nullptr || suffix == archive) return false;
    const size_t stem_size = static_cast<size_t>(suffix - archive);
    if (stem_size >= sizeof(current)) return false;
    std::memcpy(current, archive, stem_size);
    current[stem_size] = '\0';
    if (!filesystem.exists("/sigap")) return true;
    File dir = filesystem.open("/sigap", FILE_READ);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }
    File entry;
    while ((entry = dir.openNextFile())) {
      const char* full = entry.path();
      if (full == nullptr) { entry.close(); dir.close(); return false; }
      const char* base = std::strrchr(full, '/');
      base = base == nullptr ? full : base + 1;
      const char* dot = std::strchr(base, '.');
      bool boot_file = false;
      if (base[0] != '\0' && std::strncmp(base, "web-b", 5U) == 0 && dot != nullptr) {
        const size_t stem_length = static_cast<size_t>(dot - base);
        if (stem_length == 37U) {
          boot_file = true;
          for (const char* p = base + 5; boot_file && p < dot; ++p)
            boot_file = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');

        }
      }
      if (!boot_file && dot != nullptr && static_cast<size_t>(dot - base) == 22U) {
        boot_file = true;
        for (const char* p = base; boot_file && p < dot; ++p)
          boot_file = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
        if (boot_file) {
          const char* suffix = dot + 1;
          boot_file = std::strchr("scgbdak", dot[1]) != nullptr &&
              (suffix[1] == '\0' || suffix[1] == '.');
        }
        if (boot_file) boot_file = dot[-1] == 'A' || dot[-1] == 'Q' || dot[-1] == 'g' || dot[-1] == 'w';
      }
      constexpr char kFixedSyncPrefix[] = "web-sync-v3.";
      const bool fixed_sync_file = std::strncmp(
          base, kFixedSyncPrefix, sizeof(kFixedSyncPrefix) - 1U) == 0;
      if ((boot_file || fixed_sync_file) &&
          std::strncmp(full, current, stem_size) != 0 && !entry.isDirectory()) {
        const uint64_t size = entry.size();
        if (retained_legacy_bytes_ > std::numeric_limits<uint64_t>::max() - size) {
          entry.close(); dir.close(); return false;
        }
        retained_legacy_bytes_ += size;
      }
      entry.close();
    }
    dir.close();
    return true;
  }
  bool account_read_only_file(fs::FS& filesystem, const char* path) noexcept {
    if (!filesystem.exists(path)) return true;
    File file = filesystem.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      return false;
    }
    const uint64_t size = file.size();
    file.close();
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    if (retained_legacy_bytes_ > max - size) return false;
    retained_legacy_bytes_ += size;
    return true;
  }
#endif
  CapacityProbe live_probe_{nullptr};
  void* live_context_{nullptr};
  uint64_t retained_legacy_bytes_{0U};
};
} // namespace sigap::storage
