#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "sigap/build/build_config.h"
#include "acquisition/record.h"
#include "storage/archive_codec.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"

namespace sigap {

// Current readings, not a pending archive. Only one wire body is immutable.
class LatestSampleRuntime final {
 public:
  static constexpr std::size_t kSensorCount{13U};
  static constexpr uint64_t kMaxStartAgeUs{3000000U};
  static constexpr uint64_t kSendPeriodUs{build::kLiveSnapshotPeriodUs};

  bool begin(const acquisition::RecordContext& context,
             acquisition::SequenceAllocator& sequence, uint64_t boot_us,
             std::string_view version, uint8_t reset_reason) noexcept {
    if (bound_ || !context.device_id.present || !context.boot_id.present ||
        !context.config_ref.present || version.empty() || version.size() >= 64U ||
        !sequence.allocate(boot_sequence_)) return false;
    context_ = context;
    boot_us_ = boot_us;
    version_ = version;
    reset_reason_ = reset_reason;
    bound_ = true;
    boot_unattempted_ = true;
    return true;
  }

  void observe(const acquisition::SampleRecord& sample) noexcept {
    const auto index = static_cast<std::size_t>(sample.header.sensor);
    if (!bound_ || index >= slots_.size() ||
        sample.header.device_id.bytes != context_.device_id.bytes ||
        sample.header.boot_id.bytes != context_.boot_id.bytes ||
        !sample.header.device_id.present || !sample.header.boot_id.present) {
      ++rejected_; return;
    }
    auto& slot = slots_[index];
    if (slot.present && sample.header.sequence <= slot.sample.header.sequence) {
      ++rejected_; return;
    }
    if (slot.present && (!slot.attempted || slot.sample.header.sequence > slot.last_attempted))
      ++superseded_;
    slot.sample = sample;
    slot.present = true;
    ++observed_;
  }

  // Call only at a ready send boundary, not while waiting for network/backoff.
  bool prepare(uint64_t now_us) noexcept {
    if (!bound_) return false;
    if (pending_) return true;
    if (boot_unattempted_) {
      boot_unattempted_ = false;
      if (!encode_boot() || !builder_.build(record_, batch_)) { ++rejected_; return false; }
      pending_ = true; ++batches_built_; last_start_age_us_ = 0U;
      return true;
    }
    if (sample_started_ && (now_us < last_sample_start_us_ ||
        now_us - last_sample_start_us_ < kSendPeriodUs)) return false;
    telemetry_archive::BatchJsonBuilder::SampleGroupBuilder group;
    uint64_t max_age{0U};
    for (auto& slot : slots_) {
      if (!slot.present || (slot.attempted && slot.sample.header.sequence <= slot.last_attempted)) continue;
      slot.last_attempted = slot.sample.header.sequence;
      slot.attempted = true;
      if (now_us < slot.sample.header.monotonic_us) { ++rejected_; continue; }
      const auto age = now_us - slot.sample.header.monotonic_us;
      if (age > kMaxStartAgeUs) { ++expired_; continue; }
      if (!storage::RecordCodec::encode_sample(slot.sample, record_)) { ++rejected_; continue; }
      if (group.count() == 0U &&
          !group.begin(batch_, record_.identity, static_cast<uint16_t>(kSensorCount))) {
        ++rejected_; return false;
      }
      if (group.add(record_) != telemetry_archive::BatchJsonBuilder::SampleGroupBuilder::AddStatus::Added) {
        ++discarded_records_; continue;
      }
      if (age > max_age) max_age = age;
    }
    if (group.count() == 0U || !group.finish(batch_)) return false;
    last_start_age_us_ = max_age;
    last_sample_start_us_ = now_us;
    sample_started_ = true;
    pending_ = true;
    ++batches_built_;
    return true;
  }

  telemetry_archive::AckPersistStatus apply_ack(const telemetry_archive::Ack& ack) noexcept {
    if (!pending_) return telemetry_archive::AckPersistStatus::Retryable;
    if (!telemetry_archive::ack_matches(batch_, ack)) return telemetry_archive::AckPersistStatus::Invalid;
    records_delivered_ += batch_.record_count;
    ++batches_delivered_;
    retire();
    return telemetry_archive::AckPersistStatus::Durable;
  }

  // Invoke only after the transport no longer borrows the frozen wire body.
  void discard_unconfirmed() noexcept {
    if (!pending_) return;
    discarded_records_ += batch_.record_count;
    ++discarded_batches_;
    retire();
  }

  bool pending() const noexcept { return pending_; }
  const telemetry_archive::ImmutableBatch& batch() const noexcept { return batch_; }
  std::size_t retained() const noexcept {
    std::size_t count{0U}; for (const auto& slot : slots_) if (slot.present) ++count; return count;
  }
  uint64_t observed() const noexcept { return observed_; }
  uint64_t superseded() const noexcept { return superseded_; }
  uint64_t expired() const noexcept { return expired_; }
  uint64_t rejected() const noexcept { return rejected_; }
  uint64_t discarded_records() const noexcept { return discarded_records_; }
  uint64_t discarded_batches() const noexcept { return discarded_batches_; }
  uint64_t batches_built() const noexcept { return batches_built_; }
  uint64_t batches_delivered() const noexcept { return batches_delivered_; }
  uint64_t records_delivered() const noexcept { return records_delivered_; }
  uint64_t last_start_age_us() const noexcept { return last_start_age_us_; }

 private:
  struct Slot {
    acquisition::SampleRecord sample{};
    uint64_t last_attempted{0U};
    bool present{false};
    bool attempted{false};
  };
  bool encode_boot() noexcept {
    const storage::ArchiveIdentity identity{context_.device_id, context_.boot_id,
                                           boot_sequence_, storage::ArchiveRecordKind::Metadata};
    storage::PayloadWriter writer{nullptr,0U};
    if (!storage::RecordCodec::begin(identity,record_,writer)) return false;
    const auto& config = context_.config_ref;
    return writer.u8(static_cast<uint8_t>(storage::ArchiveMetadataType::Boot)) &&
        writer.u64(boot_us_) && writer.u8(0U) && writer.i64(0) &&
        writer.u8(static_cast<uint8_t>(acquisition::TimeQuality::Unsynced)) &&
        writer.u32(config.schema_version) && writer.u64(config.config_version) &&
        writer.u8(config.present ? 1U : 0U) && writer.bytes(config.hash.data(),config.hash.size()) &&
        writer.u16(0U) && writer.u8(static_cast<uint8_t>(version_.size())) &&
        writer.bytes(reinterpret_cast<const uint8_t*>(version_.data()),version_.size()) &&
        writer.u8(reset_reason_) && storage::RecordCodec::finish(record_,writer);
  }
  void retire() noexcept {
    pending_ = false;
    batch_.body_size = 0U; batch_.record_count = 0U; batch_.batch_id_size = 0U;
    batch_.attempted = false; batch_.source.present = false;
  }
  acquisition::RecordContext context_{};
  std::array<Slot,kSensorCount> slots_{};
  storage::ImmutableRecordBytes record_{};
  telemetry_archive::BatchJsonBuilder builder_{};
  telemetry_archive::ImmutableBatch batch_{};
  std::string_view version_{};
  uint64_t boot_sequence_{0U}, boot_us_{0U}, last_start_age_us_{0U}, last_sample_start_us_{0U};
  uint64_t observed_{0U}, superseded_{0U}, expired_{0U}, rejected_{0U};
  uint64_t discarded_records_{0U}, discarded_batches_{0U};
  uint64_t batches_built_{0U}, batches_delivered_{0U}, records_delivered_{0U};
  uint8_t reset_reason_{0U};
  bool bound_{false}, boot_unattempted_{false}, pending_{false}, sample_started_{false};
};
static_assert(sizeof(LatestSampleRuntime) <= 56U*1024U,"latest runtime bounded to one body and readings");
} // namespace sigap
