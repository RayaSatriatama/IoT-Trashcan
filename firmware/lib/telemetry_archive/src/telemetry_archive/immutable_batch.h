#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "acquisition/acquisition_types.h"
#include "config/config_types.h"
#include "telemetry/sha256.h"

namespace sigap::telemetry_archive {

inline constexpr std::size_t kMaxBatchBytes{32U * 1024U};
inline constexpr std::size_t kMaxBatchIdBytes{96U};
inline constexpr uint16_t kMaxBatchRecords{128U};

enum class AckStatus : uint8_t { Durable, Invalid, Conflict, Timeout, SummaryV2 };

struct ImmutableBatch final {
  std::array<uint8_t, kMaxBatchBytes> body{};
  std::size_t body_size{0U};
  std::array<char, kMaxBatchIdBytes> batch_id{};
  std::size_t batch_id_size{0U};
  config::ConfigHash body_hash{};
  uint16_t record_count{0U};
  acquisition::DeviceId device_id{};
  bool attempted{false};

  bool prepare(std::string_view id, const uint8_t* bytes, std::size_t size,
               uint16_t count) noexcept {
    if (attempted || id.empty() || id.size() >= batch_id.size() || bytes == nullptr ||
        size == 0U || size > body.size() || count == 0U ||
        count > kMaxBatchRecords) return false;
    std::copy(id.begin(), id.end(), batch_id.begin());
    batch_id_size = id.size();
    std::copy(bytes, bytes + size, body.begin());
    body_size = size;
    record_count = count;
    body_hash = telemetry_contract::sha256(std::string_view(
        reinterpret_cast<const char*>(body.data()), body_size));
    attempted = true;
    return true;
  }
  bool prepare_in_place(std::string_view id, std::size_t size,
                        uint16_t count) noexcept {
    if (attempted || id.empty() || id.size() >= batch_id.size() ||
        size == 0U || size > body.size() || count == 0U ||
        count > kMaxBatchRecords) return false;
    std::copy(id.begin(), id.end(), batch_id.begin());
    batch_id_size = id.size();
    body_size = size;
    record_count = count;
    body_hash = telemetry_contract::sha256(exact_body());
    attempted = true;
    return true;
  }
  std::string_view id() const noexcept { return std::string_view(batch_id.data(), batch_id_size); }
  std::string_view exact_body() const noexcept {
    return std::string_view(reinterpret_cast<const char*>(body.data()), body_size);
  }
  bool body_hash_matches() const noexcept {
    if (!attempted || body_size == 0U || body_size > body.size()) return false;
    return telemetry_contract::sha256(exact_body()) == body_hash;
  }
  void set_device_id(const acquisition::DeviceId& device) noexcept {
    device_id = device;
  }
};

struct Ack final {
  std::array<char, kMaxBatchIdBytes> batch_id{};
  std::size_t batch_id_size{0U};
  config::ConfigHash body_hash{};
  uint16_t record_count{0U};
  uint16_t stored_count{0U};
  uint16_t deduplicated_count{0U};
  acquisition::DeviceId device_id{};
  AckStatus status{AckStatus::Invalid};
};

inline bool ack_matches(const ImmutableBatch& batch, const Ack& ack) noexcept {
  return batch.attempted && ack.status == AckStatus::Durable &&
         batch.device_id.present && ack.device_id.present &&
         batch.device_id.bytes == ack.device_id.bytes &&
         ack.batch_id_size == batch.batch_id_size &&
         std::equal(batch.batch_id.begin(), batch.batch_id.begin() + batch.batch_id_size,
                    ack.batch_id.begin()) && ack.body_hash == batch.body_hash &&
         ack.record_count == batch.record_count &&
         static_cast<uint32_t>(ack.stored_count) + ack.deduplicated_count ==
             batch.record_count;
}

enum class JournalState : uint8_t { Pending, ServerAcked, Quarantined };

struct BatchJournalEntry final {
  ImmutableBatch batch{};
  JournalState state{JournalState::Pending};
  bool checkpointed{false};
};

template <std::size_t Capacity>
class AckJournal final {
 public:
  bool append(const ImmutableBatch& batch) noexcept {
    if (!batch.attempted || find(batch.id()) != nullptr || count_ == Capacity) return false;
    entries_[count_++] = BatchJournalEntry{batch, JournalState::Pending, false};
    return true;
  }
  bool apply_ack(const Ack& ack) noexcept {
    auto* entry = find(ack.batch_id_size == 0U
                           ? std::string_view{}
                           : std::string_view(ack.batch_id.data(), ack.batch_id_size));
    if (entry == nullptr || !ack_matches(entry->batch, ack)) return false;
    entry->state = JournalState::ServerAcked;
    entry->checkpointed = false;
    return true;
  }
  bool checkpoint() noexcept {
    for (std::size_t i = 0U; i < count_; ++i) entries_[i].checkpointed = true;
    return true;
  }
  std::size_t reclaim_checkpointed() noexcept {
    std::size_t reclaimed{0U};
    std::size_t write{0U};
    for (std::size_t read = 0U; read < count_; ++read) {
      if (entries_[read].state == JournalState::ServerAcked && entries_[read].checkpointed) {
        ++reclaimed;
        continue;
      }
      entries_[write++] = entries_[read];
    }
    count_ = write;
    return reclaimed;
  }
  const BatchJournalEntry* at(std::size_t index) const noexcept { return index < count_ ? &entries_[index] : nullptr; }
  std::size_t size() const noexcept { return count_; }
  BatchJournalEntry* find(std::string_view id) noexcept {
    for (std::size_t i = 0U; i < count_; ++i) {
      if (entries_[i].batch.id() == id) return &entries_[i];
    }
    return nullptr;
  }

 private:
  std::array<BatchJournalEntry, Capacity> entries_{};
  std::size_t count_{0U};
};

}  // namespace sigap::telemetry_archive
