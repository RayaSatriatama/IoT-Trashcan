#pragma once

#include "archive_media.h"

namespace sigap::storage {

// A selection view, not a migration: unsupported previous-boot frames stay
// original, pending, quota-accounted bytes. The caller owns the encoder scratch.
class LegacyPendingArchiveStore : public LittleFsArchiveStore {
 public:
  using Eligibility = bool (*)(const ImmutableRecordBytes&, void*) noexcept;
  static constexpr std::size_t kRecordsPerPeek{2U};

  explicit LegacyPendingArchiveStore(
      const char* path = "/sigap/archive.bin",
      const char* checkpoint = "/sigap/archive.ckp",
      const char* gap = "/sigap/gaps.bin") noexcept
      : LittleFsArchiveStore(path, checkpoint, gap) {}

  void set_selection_context(const acquisition::DeviceId& device,
                             const acquisition::BootId& boot,
                             Eligibility eligible, void* context) noexcept {
    device_ = device;
    boot_ = boot;
    eligible_ = eligible;
    context_ = context;
    reset_selection();
  }

  bool recover() noexcept override {
    reset_selection();
    ready_ = LittleFsArchiveStore::recover();
    return ready_;
  }

  ArchiveHealth health() const noexcept override {
    const auto original = LittleFsArchiveStore::health();
    if (original == ArchiveHealth::Blocked) return original;
    return retained_records_ != 0U || faulted_ ? ArchiveHealth::Degraded
                                               : original;
  }

  uint64_t retained_bytes() const noexcept { return retained_bytes_; }
  uint64_t retained_records() const noexcept { return retained_records_; }
  bool selection_faulted() const noexcept { return faulted_; }

  bool peek_pending(ImmutableRecordBytes& output) noexcept override {
#ifdef ARDUINO
    selected_ = false;
    if (!ready_ || filesystem_ == nullptr) return false;
    if (eligible_ == nullptr || !valid_id(device_) || !valid_id(boot_)) {
      return LittleFsArchiveStore::peek_pending(output);
    }
    File file = filesystem_->open(path_, FILE_READ);
    if (!file) {
      if (pending_bytes() != 0U) faulted_ = true;
      return false;
    }
    if (cursor_ > file.size() || !file.seek(cursor_, SeekSet)) {
      faulted_ = true;
      file.close();
      return false;
    }
    // At most two maximum-size frames (32 KiB) per invocation, including
    // eligibility checks. A false peek with pending bytes is NOT a drain.
    for (std::size_t count = 0U; count < kRecordsPerPeek && file.available(); ++count) {
      if (!read_frame(file, output)) {
        faulted_ = true;
        file.close();
        return false;
      }
      if (!valid_id(output.identity.device_id) ||
          output.identity.device_id.bytes != device_.bytes ||
          !valid_id(output.identity.boot_id)) {
        // Identity is a trust boundary, not an eligibility hint. Do not expose
        // invalid or foreign records to the batch builder/runtime.
        faulted_ = true;
        file.close();
        return false;
      }
      if (eligible_(output, context_)) {
        selected_ = true;
        selected_identity_ = output.identity;
        selected_hash_ = output.hash;
        file.close();
        return true;
      }
      if (output.identity.boot_id.bytes == boot_.bytes) {
        // Let the unchanged batch builder surface a current/foreign/invalid
        // producer fault. Never advance past it as though it were historical.
        faulted_ = true;
        file.close();
        return true;
      }
      cursor_ += output.size;
      retained_bytes_ += output.size;
      ++retained_records_;
    }
    file.close();
#else
    (void)output;
#endif
    return false;
  }

  bool reclaim_pending(const ArchiveIdentity& identity,
                       const config::ConfigHash& hash) noexcept override {
    // Restart ACKs may arrive before peek. Resume bounded selection, but never
    // permit a retained unsupported frame to be reclaimed by identity alone.
    if (!selected_ || !same_identity(selected_identity_, identity) ||
        selected_hash_ != hash) {
      if (!peek_pending(ack_probe_) || !selected_ ||
          !same_identity(selected_identity_, identity) || selected_hash_ != hash) {
        return false;
      }
    }
    return LittleFsArchiveStore::reclaim_pending(identity, hash);
  }

 private:
  template <typename Id>
  static bool valid_id(const Id& id) noexcept {
    if (!id.present) return false;
    for (auto byte : id.bytes) if (byte != 0U) return true;
    return false;
  }

  static bool same_identity(const ArchiveIdentity& a,
                            const ArchiveIdentity& b) noexcept {
    return a.kind == b.kind && a.sequence == b.sequence &&
           a.device_id.bytes == b.device_id.bytes && a.boot_id.bytes == b.boot_id.bytes;
  }

  void reset_selection() noexcept {
    cursor_ = 0U;
    retained_bytes_ = 0U;
    retained_records_ = 0U;
    selected_ = false;
    faulted_ = false;
  }

#ifdef ARDUINO
  static bool read_frame(File& file, ImmutableRecordBytes& record) noexcept {
    record.size = 0U;
    record.committed = false;
    if (file.read(record.bytes.data(), kArchiveHeaderBytes) !=
        static_cast<int>(kArchiveHeaderBytes)) return false;
    uint32_t payload{0U};
    PayloadReader header{record.bytes.data() + kArchivePayloadSizeOffset, sizeof(payload)};
    if (!header.u32(payload) || payload > kArchiveMaxRecordBytes -
        kArchiveHeaderBytes - kArchiveTrailerBytes) return false;
    record.size = kArchiveHeaderBytes + payload + kArchiveTrailerBytes;
    const auto remaining = record.size - kArchiveHeaderBytes;
    if (file.read(record.bytes.data() + kArchiveHeaderBytes, remaining) !=
        static_cast<int>(remaining)) return false;
    record.committed = true;
    ArchiveRecordView view{};
    if (!RecordCodec::read(record, view)) return false;
    record.identity = view.identity;
    record.hash = telemetry_contract::sha256(std::string_view(
        reinterpret_cast<const char*>(record.bytes.data()), record.size));
    return true;
  }
#endif

  acquisition::DeviceId device_{};
  acquisition::BootId boot_{};
  Eligibility eligible_{nullptr};
  void* context_{nullptr};
  std::size_t cursor_{0U};
  uint64_t retained_bytes_{0U};
  uint64_t retained_records_{0U};
  bool ready_{false};
  bool selected_{false};
  bool faulted_{false};
  ArchiveIdentity selected_identity_{};
  config::ConfigHash selected_hash_{};
  ImmutableRecordBytes ack_probe_{};
};

}  // namespace sigap::storage
