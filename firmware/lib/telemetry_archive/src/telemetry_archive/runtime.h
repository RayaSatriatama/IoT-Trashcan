#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <cstdio>
#include <string_view>

#include "immutable_batch.h"
#include "acquisition/clock.h"
#include "debug/types.h"
#include "acquisition/profiles.h"
#include "telemetry/canonical_writer.h"
#include "storage/archive_codec.h"
#include "storage/archive_store.h"

namespace sigap::telemetry_archive {

enum class BatchRuntimeStatus : uint8_t {
  Idle,
  Durable,
  Retryable,
  Invalid,
  LoggingBlocked,
};

class BatchJsonBuilder final {
 public:
  bool build(const storage::ImmutableRecordBytes& record,
             ImmutableBatch& batch) noexcept {
    batch = ImmutableBatch{};
    if (!storage::RecordCodec::valid(record)) return false;
    // Keep the record in the tail of the caller-owned batch buffer while the
    // envelope header is assembled in-place. This avoids a second 32 KiB
    // scratch object on the storage worker stack.
    constexpr std::size_t kEnvelopeReserve{1024U};
    if (kEnvelopeReserve >= batch.body.size()) return false;
    constexpr std::size_t kResultWorkspaceBytes{
        sizeof(storage::DecodedResultPayload)};
    const std::size_t workspace_reserve =
        record.identity.kind == storage::ArchiveRecordKind::Result
            ? kResultWorkspaceBytes
            : 0U;
    if (workspace_reserve >= batch.body.size() - kEnvelopeReserve) return false;
    telemetry_contract::CanonicalBuffer writer{
        reinterpret_cast<char*>(batch.body.data() + kEnvelopeReserve),
        batch.body.size() - kEnvelopeReserve - workspace_reserve};
    bool written = false;
    switch (record.identity.kind) {
      case storage::ArchiveRecordKind::Sample:
        written = write_sample(record, writer);
        break;
      case storage::ArchiveRecordKind::Result:
        written = write_result(record, writer, result_workspace(batch));
        break;
      case storage::ArchiveRecordKind::Event:
        written = write_event(record, writer);
        break;
      case storage::ArchiveRecordKind::Metadata:
        written = write_metadata(record, writer);
        break;
    }
    if (!written || writer.size() == 0U) return false;
    const auto record_hash = telemetry_contract::sha256(writer.view());
    if (!insert_record_hash(writer, record_hash)) return false;
    const std::size_t record_size = writer.size();
    char id[96]{};
    const int id_size = make_batch_id(record.identity, id, sizeof(id));
    if (id_size <= 0) return false;
    telemetry_contract::CanonicalBuffer envelope{
        reinterpret_cast<char*>(batch.body.data()), batch.body.size()};
    if (!append_envelope_prefix(envelope, record.identity, id,
                                static_cast<std::size_t>(id_size),
                                record.identity.kind)) {
      return false;
    }
    const std::size_t prefix_size = envelope.size();
    if (prefix_size + record_size > batch.body.size()) return false;
    std::memmove(batch.body.data() + prefix_size,
                 batch.body.data() + kEnvelopeReserve, record_size);
    envelope.set_size(prefix_size + record_size);
    if (!append_envelope_suffix(envelope, record.identity.kind)) return false;
    const std::size_t body_size = envelope.size();
    if (!batch.prepare_in_place(std::string_view(id, static_cast<std::size_t>(id_size)),
                                body_size, 1U)) {
      return false;
    }
    batch.set_device_id(record.identity.device_id);
    return batch.bind_source(record.identity.device_id, record.identity.boot_id,
                             record.identity.sequence,
                             static_cast<uint8_t>(record.identity.kind),
                             record.hash);
  }

  // Bounded multi-record sample group composer.
  //
  // It serializes up to kMaxBatchRecords sample records that share one device
  // identity and one boot identity into a single ImmutableBatch body. Records
  // are added one at a time from the caller's own scratch record, so no array
  // of 16 KiB records is ever materialized. A record that does not fit ends the
  // group (Full) instead of corrupting it; a record of a different kind,
  // device or boot is rejected (Invalid) and is never consumed, so the caller
  // can keep FIFO order and hand that record to its own batch.
  //
  // A group of one record produces bytes identical to build(single).
  class SampleGroupBuilder final {
   public:
    enum class AddStatus : uint8_t { Added, Full, Invalid };

    bool begin(ImmutableBatch& batch, const storage::ArchiveIdentity& first,
               uint16_t max_records) noexcept {
      open_ = false;
      batch_ = &batch;
      // Reset in place instead of assigning a 32 KiB temporary: the batch body
      // is instance-owned and this runs from the main loop.
      batch.attempted = false;
      batch.body_size = 0U;
      batch.record_count = 0U;
      batch.batch_id_size = 0U;
      batch.body_hash = {};
      batch.device_id = {};
      batch.source = {};
      count_ = 0U;
      cursor_ = kHeaderReserve;
      first_ = first;
      first_hash_ = {};

      id_size_ = 0;
      if (!first.device_id.present || !first.boot_id.present) return false;
      if (first.kind != storage::ArchiveRecordKind::Sample) return false;
      if (max_records == 0U || max_records > kMaxBatchRecords) return false;
      max_records_ = max_records;
      id_size_ = make_batch_id(first, id_, sizeof(id_));
      if (id_size_ <= 0) return false;
      open_ = true;
      return true;
    }

    AddStatus add(const storage::ImmutableRecordBytes& record) noexcept {
      if (!open_ || batch_ == nullptr) return AddStatus::Invalid;
      if (count_ >= max_records_) return AddStatus::Full;
      if (!storage::RecordCodec::valid(record)) return AddStatus::Invalid;
      if (record.identity.kind != storage::ArchiveRecordKind::Sample) {
        return AddStatus::Invalid;
      }
      if (record.identity.device_id.bytes != first_.device_id.bytes ||
          record.identity.boot_id.bytes != first_.boot_id.bytes) {
        return AddStatus::Invalid;
      }
      // FIFO append order need not be capture order; telemetry-v3 requires unique IDs.
      for (std::size_t index = 0U; index < count_; ++index) {
        if (record.identity.sequence == sequences_[index]) return AddStatus::Invalid;
      }
      const std::size_t separator = count_ == 0U ? 0U : 1U;
      if (cursor_ + separator >= batch_->body.size()) return AddStatus::Full;
      // Reject before the subtraction below can underflow: the record area must
      // leave at least one usable byte after the reserved suffix. Without this
      // guard an exact-fit cursor (remaining == kSuffixReserve) wraps the
      // unsigned capacity to SIZE_MAX and the writer would run past the body.
      if (batch_->body.size() - cursor_ - separator <= kSuffixReserve) {
        return AddStatus::Full;
      }
      const std::size_t capacity =
          batch_->body.size() - cursor_ - separator - kSuffixReserve;
      if (capacity == 0U) return AddStatus::Full;
      telemetry_contract::CanonicalBuffer writer{
          reinterpret_cast<char*>(batch_->body.data() + cursor_ + separator),
          capacity};
      if (!write_sample(record, writer)) {
        return writer.size() == 0U ? AddStatus::Invalid : AddStatus::Full;
      }
      const auto record_hash = telemetry_contract::sha256(writer.view());
      if (!insert_record_hash(writer, record_hash)) return AddStatus::Full;
      if (separator != 0U) batch_->body[cursor_] = ',';
      cursor_ += separator + writer.size();
      sequences_[count_++] = record.identity.sequence;
      if (count_ == 1U) first_hash_ = record.hash;
      return AddStatus::Added;
    }

    bool finish(ImmutableBatch& batch) noexcept {
      if (!open_ || batch_ != &batch || count_ == 0U) return false;
      char header[kHeaderReserve]{};
      telemetry_contract::CanonicalBuffer envelope{header, sizeof(header)};
      if (!append_envelope_prefix(envelope, first_, id_,
                                  static_cast<std::size_t>(id_size_),
                                  storage::ArchiveRecordKind::Sample, count_)) {
        return false;
      }
      const std::size_t header_size = envelope.size();
      char tail[kSuffixReserve]{};
      telemetry_contract::CanonicalBuffer suffix{tail, sizeof(tail)};
      if (!append_envelope_suffix(suffix, storage::ArchiveRecordKind::Sample)) {
        return false;
      }
      const std::size_t records_size = cursor_ - kHeaderReserve;
      const std::size_t total = header_size + records_size + suffix.size();
      if (total > batch.body.size()) return false;
      std::memmove(batch.body.data() + header_size,
                   batch.body.data() + kHeaderReserve, records_size);
      std::memcpy(batch.body.data(), header, header_size);
      std::memcpy(batch.body.data() + header_size + records_size, tail,
                  suffix.size());
      if (!batch.prepare_in_place(std::string_view(id_, static_cast<std::size_t>(id_size_)),
                                  total, count_)) {
        return false;
      }
      batch.set_device_id(first_.device_id);
      open_ = false;
      return batch.bind_source(first_.device_id, first_.boot_id, first_.sequence,
                               static_cast<uint8_t>(storage::ArchiveRecordKind::Sample),
                               first_hash_);
    }

    std::size_t count() const noexcept { return count_; }
    std::string_view id() const noexcept {
      return std::string_view(id_, static_cast<std::size_t>(id_size_));
    }
    bool open() const noexcept { return open_; }

   private:
    // Worst case header: schema/device/batch-id/record-count/samples prefix is
    // under 256 bytes for the 96-byte batch-id bound; the sample suffix is
    // under 64 bytes. Both are checked before use.
    static constexpr std::size_t kHeaderReserve{256U};
    static constexpr std::size_t kSuffixReserve{64U};

    ImmutableBatch* batch_{nullptr};
    storage::ArchiveIdentity first_{};
    config::ConfigHash first_hash_{};
    char id_[kMaxBatchIdBytes]{};
    int id_size_{0};
    std::size_t count_{0U};
    std::size_t cursor_{0U};
    uint16_t max_records_{0U};
    std::array<uint64_t, kMaxBatchRecords> sequences_{};
    bool open_{false};
  };

 private:
  static storage::DecodedResultPayload& result_workspace(
      ImmutableBatch& batch) noexcept {
    constexpr std::size_t kWorkspaceBytes{
        sizeof(storage::DecodedResultPayload)};
    auto* address = batch.body.data() + batch.body.size() - kWorkspaceBytes;
    return *::new (static_cast<void*>(address)) storage::DecodedResultPayload{};
  }

  static bool append_string(telemetry_contract::CanonicalBuffer& out,
                            std::string_view value) noexcept {
    return out.append('"') && telemetry_contract::detail::append_json_string_content(out, value) &&
           out.append('"');
  }
  static bool key(telemetry_contract::CanonicalBuffer& out,
                  std::string_view value) noexcept {
    return append_string(out, value) && out.append(':');
  }
  static bool uint64_string(telemetry_contract::CanonicalBuffer& out,
                            uint64_t value) noexcept {
    return out.append('"') && telemetry_contract::detail::append_uint64(out, value) &&
           out.append('"');
  }
  static bool hex(telemetry_contract::CanonicalBuffer& out,
                  const uint8_t* bytes, std::size_t size) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    if (!out.append('"')) return false;
    for (std::size_t i = 0U; i < size; ++i) {
      if (!out.append(digits[bytes[i] >> 4U]) || !out.append(digits[bytes[i] & 0x0fU])) {
        return false;
      }
    }
    return out.append('"');
  }
  static bool prefixed_hash(telemetry_contract::CanonicalBuffer& out,
                            const config::ConfigHash& hash) noexcept {
    return out.append('"') && out.append("sha256:") &&
           append_hex_unquoted(out, hash.data(), hash.size()) && out.append('"');
  }
  static bool append_hex_unquoted(telemetry_contract::CanonicalBuffer& out,
                                  const uint8_t* bytes, std::size_t size) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0U; i < size; ++i) {
      if (!out.append(digits[bytes[i] >> 4U]) || !out.append(digits[bytes[i] & 0x0fU])) {
        return false;
      }
    }
    return true;
  }
  static const char* stream_name(acquisition::StreamId stream) noexcept {
    switch (stream) {
      case acquisition::StreamId::Organic: return "organic";
      case acquisition::StreamId::Inorganic: return "inorganic";
      case acquisition::StreamId::B3: return "b3";
      case acquisition::StreamId::Ambient: return "ambient";
    }
    return "b3";
  }
  static const char* result_stream_name(uint8_t stream) noexcept {
    switch (static_cast<acquisition::StreamId>(stream)) {
      case acquisition::StreamId::Organic: return "organic";
      case acquisition::StreamId::Inorganic: return "inorganic";
      case acquisition::StreamId::B3: return "b3";
      case acquisition::StreamId::Ambient: break;
    }
    return "b3";
  }
  static bool append_device(telemetry_contract::CanonicalBuffer& out,
                            const acquisition::DeviceId& id) noexcept {
    if (!id.present) return false;
    return hex(out, id.bytes.data(), id.bytes.size());
  }
  static bool append_envelope_prefix(telemetry_contract::CanonicalBuffer& out,
                                     const storage::ArchiveIdentity& identity,
                                     const char* batch_id,
                                     std::size_t batch_id_size,
                                     storage::ArchiveRecordKind kind,
                                     uint16_t record_count = 1U) noexcept {
    if (!out.append("{\"schema_version\":3,\"device_id\":") ||
        !append_device(out, identity.device_id) ||
        !out.append(",\"batch_id\":") ||
        !append_string(out, std::string_view(batch_id, batch_id_size)) ||
        !out.append(",\"record_count\":") ||
        !append_unsigned(out, record_count) ||
        !out.append(",\"samples\":")) return false;
    if (kind == storage::ArchiveRecordKind::Sample) return out.append("[");
    if (!out.append("[],\"results\":")) return false;
    if (kind == storage::ArchiveRecordKind::Result) return out.append("[");
    if (!out.append("[],\"events\":")) return false;
    if (kind == storage::ArchiveRecordKind::Event) return out.append("[");
    if (!out.append("[],\"metadata\":")) return false;
    if (kind == storage::ArchiveRecordKind::Metadata) return out.append("[");
    return false;
  }
  static bool append_envelope_suffix(telemetry_contract::CanonicalBuffer& out,
                                     storage::ArchiveRecordKind kind) noexcept {
    // Prefix leaves the selected array open. Close it and append the remaining
    // arrays in deterministic schema order.
    if (!out.append(']')) return false;
    if (kind == storage::ArchiveRecordKind::Sample) {
      return out.append(",\"results\":[],\"events\":[],\"metadata\":[]}");
    }
    if (kind == storage::ArchiveRecordKind::Result) {
      return out.append(",\"events\":[],\"metadata\":[]}");
    }
    if (kind == storage::ArchiveRecordKind::Event) {
      return out.append(",\"metadata\":[]}");
    }
    return out.append('}');
  }
  static const char* sensor_name(acquisition::SensorId sensor) noexcept {
    switch (sensor) {
      case acquisition::SensorId::Mq4: return "mq4";
      case acquisition::SensorId::Mq135: return "mq135";
      case acquisition::SensorId::Mq7: return "mq7";
      case acquisition::SensorId::Mq8: return "mq8";
      case acquisition::SensorId::Hx711Organic: return "hx711_organic";
      case acquisition::SensorId::Hx711Inorganic: return "hx711_inorganic";
      case acquisition::SensorId::Hx711B3: return "hx711_b3";
      case acquisition::SensorId::UltrasonicOrganic: return "ultrasonic_organic";
      case acquisition::SensorId::UltrasonicInorganic: return "ultrasonic_inorganic";
      case acquisition::SensorId::UltrasonicB3: return "ultrasonic_b3";
      case acquisition::SensorId::Ds18b20B3: return "ds18b20_b3";
      case acquisition::SensorId::Sht31_44: return "sht31_44";
      case acquisition::SensorId::Sht31_45: return "sht31_45";
    }
    return "mq4";
  }
  static const char* quality_name(acquisition::SampleQuality quality) noexcept {
    switch (quality) {
      case acquisition::SampleQuality::Ready: return "READY";
      case acquisition::SampleQuality::WarmingUp: return "WARMING_UP";
      case acquisition::SampleQuality::HeaterPhase: return "HEATER_PHASE";
      case acquisition::SampleQuality::Uncalibrated: return "UNCALIBRATED";
      case acquisition::SampleQuality::Calibrating: return "CALIBRATING";
      case acquisition::SampleQuality::Timeout: return "TIMEOUT";
      case acquisition::SampleQuality::Disconnected: return "DISCONNECTED";
      case acquisition::SampleQuality::CrcError: return "CRC_ERROR";
      case acquisition::SampleQuality::Stale: return "STALE";
      case acquisition::SampleQuality::OutOfRange: return "OUT_OF_RANGE";
    }
    return "TIMEOUT";
  }
  static const char* time_quality(acquisition::TimeQuality quality) noexcept {
    switch (quality) {
      case acquisition::TimeQuality::Synchronized: return "SYNCED";
      case acquisition::TimeQuality::Estimated: return "RECONSTRUCTED";
      case acquisition::TimeQuality::Unsynced: return "UNSYNCED";
    }
    return "UNSYNCED";
  }
  static const char* reason_name(acquisition::SampleReason reason) noexcept {
    switch (reason) {
      case acquisition::SampleReason::None: return "NONE";
      case acquisition::SampleReason::DataReady: return "DATA_READY";
      case acquisition::SampleReason::NotReady: return "NOT_READY";
      case acquisition::SampleReason::Timeout: return "TIMEOUT";
      case acquisition::SampleReason::Disconnected: return "DISCONNECTED";
      case acquisition::SampleReason::CrcError: return "CRC_ERROR";
      case acquisition::SampleReason::InvalidProfile: return "INVALID_PROFILE";
      case acquisition::SampleReason::HeaterPhase: return "HEATER_PHASE";
      case acquisition::SampleReason::ConversionPending: return "CONVERSION_PENDING";
      case acquisition::SampleReason::OutOfRange: return "OUT_OF_RANGE";
      case acquisition::SampleReason::ProfileMissing: return "PROFILE_MISSING";
    }
    return "UNKNOWN";
  }
  static bool append_model_id(telemetry_contract::CanonicalBuffer& out,
                              const char* data, std::size_t size) noexcept {
    std::size_t length{0U};
    while (length < size && data[length] != '\0') ++length;
    return append_string(out, std::string_view(data, length));
  }
  static const char* profile_kind_name(uint8_t kind) noexcept {
    switch (static_cast<acquisition::ProfileKind>(kind)) {
      case acquisition::ProfileKind::Calibration: return "CALIBRATION";
      case acquisition::ProfileKind::Adc: return "ADC";
      case acquisition::ProfileKind::Fis: return "FIS";
      case acquisition::ProfileKind::Safety: return "SAFETY";
      case acquisition::ProfileKind::Heater: return "HEATER";
      case acquisition::ProfileKind::Capacity: return "CAPACITY";
    }
    return "CALIBRATION";
  }
  static const char* profile_target_name(uint32_t target) noexcept {
    switch (static_cast<acquisition::ProfileTarget>(target)) {
      case acquisition::ProfileTarget::Mq4: return "mq4";
      case acquisition::ProfileTarget::Mq135: return "mq135";
      case acquisition::ProfileTarget::Mq7: return "mq7";
      case acquisition::ProfileTarget::Mq8: return "mq8";
      case acquisition::ProfileTarget::Hx711Organic: return "hx711_organic";
      case acquisition::ProfileTarget::Hx711Inorganic: return "hx711_inorganic";
      case acquisition::ProfileTarget::Hx711B3: return "hx711_b3";
      case acquisition::ProfileTarget::UltrasonicOrganic: return "ultrasonic_organic";
      case acquisition::ProfileTarget::UltrasonicInorganic: return "ultrasonic_inorganic";
      case acquisition::ProfileTarget::UltrasonicB3: return "ultrasonic_b3";
      case acquisition::ProfileTarget::Ds18b20B3: return "ds18b20_b3";
      case acquisition::ProfileTarget::Sht31_44: return "sht31_44";
      case acquisition::ProfileTarget::Sht31_45: return "sht31_45";
      case acquisition::ProfileTarget::Unknown: break;
    }
    return "unknown";
  }
  static bool append_profile_ref(telemetry_contract::CanonicalBuffer& out,
                                const storage::DecodedProfileRef& ref) noexcept {
    return out.append('{') && key(out, "hash") && prefixed_hash(out, ref.hash) &&
           out.append(',') && key(out, "kind") && append_string(out, profile_kind_name(ref.kind)) &&
           out.append(',') && key(out, "schema_version") && append_unsigned(out, ref.schema_version) &&
           out.append(',') && key(out, "target") && append_string(out, profile_target_name(ref.target_id)) &&
           out.append(',') && key(out, "version") && uint64_string(out, ref.profile_version) &&
           out.append('}');
  }
  static bool append_profile_refs(telemetry_contract::CanonicalBuffer& out,
                                  const storage::DecodedProfileRef* refs,
                                  std::size_t count) noexcept {
    if (!out.append('[')) return false;
    for (std::size_t index = 0U; index < count; ++index) {
      if (index != 0U && !out.append(',')) return false;
      if (!append_profile_ref(out, refs[index])) return false;
    }
    return out.append(']');
  }
  static bool append_sample_profile_refs(telemetry_contract::CanonicalBuffer& out,
                                         const acquisition::ProfileRef& ref) noexcept {
    if (!ref.present) return out.append("[]");
    storage::DecodedProfileRef decoded{};
    decoded.kind = static_cast<uint8_t>(ref.kind);
    decoded.schema_version = ref.schema_version;
    decoded.profile_version = ref.profile_version;
    decoded.target_id = ref.target_id;
    decoded.hash = ref.hash;
    decoded.present = true;
    return append_profile_refs(out, &decoded, 1U);
  }
  static bool append_common(telemetry_contract::CanonicalBuffer& out,
                            const storage::ArchiveIdentity& identity,
                            uint64_t monotonic_us,
                            const acquisition::ConfigRef& config,
                            std::string_view kind) noexcept {
    char record_id[96]{};
    const int record_id_size = make_id(identity, record_id, sizeof(record_id));
    if (record_id_size <= 0 ||
        !out.append('{') || !key(out, "boot_id") || !hex(out, identity.boot_id.bytes.data(), identity.boot_id.bytes.size()) ||
        !out.append(',') || !key(out, "config_ref") || !out.append('{') ||
        !key(out, "config_hash") || !prefixed_hash(out, config.hash) || !out.append(',') ||
        !key(out, "config_version") || !uint64_string(out, config.config_version) || !out.append(',') ||
        !key(out, "schema_version") || !append_unsigned(out, config.schema_version) || !out.append('}') || !out.append(',') ||
        !key(out, "device_id") || !hex(out, identity.device_id.bytes.data(), identity.device_id.bytes.size()) ||
        !out.append(',') || !key(out, "kind") || !append_string(out, kind) || !out.append(',') ||
        !key(out, "record_id") ||
        !append_string(out, std::string_view(record_id, static_cast<std::size_t>(record_id_size))) || !out.append(',') ||
        !key(out, "monotonic_us") || !uint64_string(out, monotonic_us) || !out.append(',') ||
        !key(out, "profile_refs")) return false;
    return true;
  }
  static bool append_config_ref(telemetry_contract::CanonicalBuffer& out,
                                const acquisition::ConfigRef& config) noexcept {
    return out.append('{') && key(out, "config_hash") && prefixed_hash(out, config.hash) &&
           out.append(',') && key(out, "config_version") &&
           uint64_string(out, config.config_version) && out.append(',') &&
           key(out, "schema_version") && append_unsigned(out, config.schema_version) &&
           out.append('}');
  }
  static bool append_config_ref(telemetry_contract::CanonicalBuffer& out,
                                const storage::DecodedConfigRef& config) noexcept {
    return out.append('{') && key(out, "config_hash") && prefixed_hash(out, config.hash) &&
           out.append(',') && key(out, "config_version") &&
           uint64_string(out, config.config_version) && out.append(',') &&
           key(out, "schema_version") && append_unsigned(out, config.schema_version) &&
           out.append('}');
  }
  static bool append_record_id(telemetry_contract::CanonicalBuffer& out,
                               const storage::ArchiveIdentity& identity) noexcept {
    char record_id[96]{};
    const int size = make_id(identity, record_id, sizeof(record_id));
    return size > 0 && append_string(out, std::string_view(record_id,
                                                            static_cast<std::size_t>(size)));
  }
  static bool append_common(telemetry_contract::CanonicalBuffer& out,
                            const storage::ArchiveIdentity& identity,
                            uint64_t monotonic_us,
                            const storage::DecodedConfigRef& config,
                            std::string_view kind) noexcept {
    acquisition::ConfigRef converted{};
    converted.present = config.present;
    converted.schema_version = config.schema_version;
    converted.config_version = config.config_version;
    converted.hash = config.hash;
    return append_common(out, identity, monotonic_us, converted, kind);
  }
  static bool append_sample_raw(telemetry_contract::CanonicalBuffer& out,
                                const acquisition::SampleRecord& sample) noexcept {
    if (!sample.raw.present) return out.append("null");
    if (sample.header.kind == acquisition::SampleKind::Hx711Counts) {
      return out.append("{\"counts\":") && append_signed(out, sample.raw.signed_counts) && out.append('}');
    }
    if (sample.header.kind == acquisition::SampleKind::MqAdc) {
      if (sample.header.sensor != acquisition::SensorId::Mq7)
        return out.append("{\"adc_counts\":") && append_unsigned(out, sample.raw.adc_counts) && out.append('}');
      if (!sample.raw.heater_phase_available) return false;
      const char* phase = "FAULT";
      switch (sample.raw.heater_phase) {
        case acquisition::Mq7HeaterPhase::Preheat: phase = "PREHEAT"; break;
        case acquisition::Mq7HeaterPhase::HighClean: phase = "HIGH_CLEAN"; break;
        case acquisition::Mq7HeaterPhase::LowDetect: phase = "LOW_DETECT"; break;
        case acquisition::Mq7HeaterPhase::Fault: phase = "FAULT"; break;
      }
      return out.append("{\"adc_counts\":") && append_unsigned(out, sample.raw.adc_counts) &&
             out.append(",\"heater_phase\":\"") && out.append(phase) && out.append("\"}");
    }
    if (sample.header.kind == acquisition::SampleKind::UltrasonicEcho) {
      return out.append("{\"echo_us\":") && append_unsigned(out, sample.raw.distance_us) && out.append('}');
    }
    if (sample.header.kind == acquisition::SampleKind::Ds18b20) {
      if (!sample.raw.ds_wire_evidence) return out.append("null");
      return out.append("{\"conversion_complete\":") &&
             out.append(sample.raw.conversion_complete ? "true" : "false") &&
             out.append(",\"crc_valid\":") &&
             out.append(sample.raw.crc_valid ? "true" : "false") &&
             out.append(",\"resolution_bits\":") &&
             append_unsigned(out, sample.raw.ds_resolution_bits) &&
             out.append(",\"rom\":") &&
             hex(out, sample.raw.ds_rom.data(), sample.raw.ds_rom.size()) &&
             out.append(",\"scratchpad_hex\":") &&
             hex(out, sample.raw.ds_scratchpad.data(), sample.raw.ds_scratchpad.size()) &&
             out.append('}');
    }
    if (sample.header.kind == acquisition::SampleKind::Sht31) {
      if (!sample.raw.sht31_ticks_available) return false;
      return out.append("{\"humidity_crc_valid\":") &&
             out.append(sample.raw.humidity_crc_valid ? "true" : "false") &&
             out.append(",\"humidity_ticks\":") &&
             append_unsigned(out, sample.raw.sht31_humidity_ticks) &&
             out.append(",\"temperature_crc_valid\":") &&
             out.append(sample.raw.temperature_crc_valid ? "true" : "false") &&
             out.append(",\"temperature_ticks\":") &&
             append_unsigned(out, sample.raw.sht31_temperature_ticks) &&
             out.append('}');
    }
    return out.append("null");
  }
  static bool append_unsigned(telemetry_contract::CanonicalBuffer& out, uint64_t value) noexcept {
    return telemetry_contract::detail::append_uint64(out, value);
  }
  static bool append_signed(telemetry_contract::CanonicalBuffer& out, int64_t value) noexcept {
    if (value >= 0) return append_unsigned(out, static_cast<uint64_t>(value));
    return out.append('-') && append_unsigned(out, static_cast<uint64_t>(-(value + 1)) + 1U);
  }
  static bool append_float(telemetry_contract::CanonicalBuffer& out, float value) noexcept {
    char text[32]{};
    const int written = std::snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
    return written > 0 && static_cast<std::size_t>(written) < sizeof(text) && out.append(std::string_view(text, static_cast<std::size_t>(written)));
  }
  static bool append_sample_converted(telemetry_contract::CanonicalBuffer& out,
                                      const acquisition::SampleRecord& sample) noexcept {
    if (sample.header.kind == acquisition::SampleKind::Hx711Counts) {
      if (!sample.converted.primary_available) return out.append("null");
      return out.append("{\"kg\":") && append_float(out, sample.converted.primary_value) && out.append('}');
    }
    if (sample.header.kind == acquisition::SampleKind::MqAdc) {
      if (!sample.converted.primary_available) return out.append("null");
      const bool calibrated_mq135 =
          sample.header.sensor == acquisition::SensorId::Mq135 &&
          sample.quality == acquisition::SampleQuality::Ready && sample.raw.present &&
          sample.raw.adc_counts > 0U &&
          sample.raw.adc_counts < acquisition::kMqAdcMaxCounts12Bit &&
          sample.converted.primary_kind == acquisition::ConvertedKind::AdcMillivolts &&
          sample.converted.secondary_available &&
          sample.converted.secondary_kind == acquisition::ConvertedKind::AdcPinMillivolts &&
          std::isfinite(sample.converted.primary_value) && sample.converted.primary_value > 0.0F &&
          std::isfinite(sample.converted.secondary_value) &&
          sample.converted.secondary_value > 0.0F &&
          sample.converted.secondary_value <= 3300.0F;
      if (sample.header.sensor == acquisition::SensorId::Mq135 &&
          sample.converted.secondary_kind == acquisition::ConvertedKind::AdcPinMillivolts &&
          !calibrated_mq135) {
        return out.append("null");
      }
      if (calibrated_mq135) {
        return out.append("{\"adc_efuse_calibrated\":1,\"adc_mv\":") &&
               append_float(out, sample.converted.primary_value) &&
               out.append(",\"adc_pin_mv\":") &&
               append_float(out, sample.converted.secondary_value) &&
               out.append(",\"sensor_ao_mv\":") &&
               append_float(out, sample.converted.primary_value) && out.append('}');
      }
      return out.append("{\"adc_mv\":") &&
             append_float(out, sample.converted.primary_value) && out.append('}');
    }
    if (sample.header.kind == acquisition::SampleKind::UltrasonicEcho) {
      if (sample.converted.primary_available &&
          sample.converted.primary_kind == acquisition::ConvertedKind::FillPercent) {
        return out.append("{\"distance_mm\":null,\"fill_percent\":") &&
               append_float(out, sample.converted.primary_value) && out.append('}');
      }
      if (!sample.converted.primary_available || !sample.converted.secondary_available) return out.append("null");
      return out.append("{\"distance_mm\":") && append_float(out, sample.converted.primary_value) &&
             out.append(",\"fill_percent\":") && append_float(out, sample.converted.secondary_value) && out.append('}');
    }
    if (sample.header.kind == acquisition::SampleKind::Sht31) {
      if (!sample.converted.primary_available || !sample.converted.secondary_available) return out.append("null");
      return out.append("{\"humidity_percent\":") && append_float(out, sample.converted.secondary_value) &&
             out.append(",\"temperature_c\":") && append_float(out, sample.converted.primary_value) && out.append('}');
    }
    if (sample.header.kind == acquisition::SampleKind::Ds18b20) {
      if (!sample.converted.primary_available) return out.append("{\"temperature_c\":null}");
      if (sample.converted.primary_kind != acquisition::ConvertedKind::Celsius) return false;
      return out.append("{\"temperature_c\":") && append_float(out, sample.converted.primary_value) && out.append('}');
    }
    return out.append("null");
  }
  static bool append_recorded_at(telemetry_contract::CanonicalBuffer& out,
                                 const acquisition::RecordedAt& recorded) noexcept {
    if (!recorded.available) return out.append("null");
    if (recorded.epoch_ms < 0) return false;
    uint64_t seconds = static_cast<uint64_t>(recorded.epoch_ms) / 1000U;
    const uint32_t milliseconds = static_cast<uint32_t>(recorded.epoch_ms % 1000);
    const uint32_t second = static_cast<uint32_t>(seconds % 60U);
    seconds /= 60U;
    const uint32_t minute = static_cast<uint32_t>(seconds % 60U);
    seconds /= 60U;
    const uint32_t hour = static_cast<uint32_t>(seconds % 24U);
    const int64_t days = static_cast<int64_t>(seconds / 24U);
    int64_t year = 1970;
    int64_t remaining_days = days;
    while (remaining_days >= (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 366 : 365)) {
      remaining_days -= (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 366 : 365);
      ++year;
    }
    uint32_t month = 1U;
    const uint32_t month_lengths[] = {31U, 28U, 31U, 30U, 31U, 30U,
                                      31U, 31U, 30U, 31U, 30U, 31U};
    while (month < 12U) {
      uint32_t length = month_lengths[month - 1U];
      if (month == 2U && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) ++length;
      if (remaining_days < static_cast<int64_t>(length)) break;
      remaining_days -= length;
      ++month;
    }
    const uint32_t day = static_cast<uint32_t>(remaining_days) + 1U;
    char text[32]{};
    const int written = std::snprintf(text, sizeof(text),
                                      "%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ",
                                      static_cast<long long>(year), month, day,
                                      hour, minute, second, milliseconds);
    return written > 0 && static_cast<std::size_t>(written) < sizeof(text) &&
           out.append('"') && out.append(std::string_view(text, static_cast<std::size_t>(written))) &&
           out.append('"');
  }
  static bool write_sample(const storage::ImmutableRecordBytes& record,
                           telemetry_contract::CanonicalBuffer& out) noexcept {
    acquisition::SampleRecord sample{};
    if (!storage::RecordCodec::decode_sample(record, sample)) return false;
    if (sample.quality == acquisition::SampleQuality::Ready &&
        (!sample.raw.present || (sample.header.kind == acquisition::SampleKind::Ds18b20 &&
                                 !sample.raw.ds_wire_evidence))) return false;
    return out.append('{') && key(out, "boot_id") &&
           hex(out, record.identity.boot_id.bytes.data(), record.identity.boot_id.bytes.size()) &&
           out.append(',') && key(out, "config_ref") && append_config_ref(out, sample.header.config_ref) &&
           out.append(',') && key(out, "converted") && append_sample_converted(out, sample) &&
           out.append(',') && key(out, "device_id") &&
           hex(out, record.identity.device_id.bytes.data(), record.identity.device_id.bytes.size()) &&
           out.append(',') && key(out, "kind") && append_string(out, "sample") &&
           out.append(',') && key(out, "monotonic_us") && uint64_string(out, sample.header.monotonic_us) &&
           out.append(',') && key(out, "profile_refs") && append_sample_profile_refs(out, sample.header.profile_ref) &&
           out.append(',') && key(out, "quality") && append_string(out, quality_name(sample.quality)) &&
           out.append(',') && key(out, "raw") && append_sample_raw(out, sample) &&
           out.append(',') && key(out, "reason") && append_string(out, reason_name(sample.reason)) &&
           out.append(',') && key(out, "record_id") && append_record_id(out, record.identity) &&
           out.append(',') && key(out, "recorded_at") && append_recorded_at(out, sample.header.recorded_at) &&
           out.append(',') && key(out, "sensor_id") && append_string(out, sensor_name(sample.header.sensor)) &&
           out.append(',') && key(out, "sequence") && uint64_string(out, record.identity.sequence) &&
           out.append(',') && key(out, "stream") && append_string(out, stream_name(sample.header.stream)) &&
           out.append(',') && key(out, "time_quality") && append_string(out, time_quality(sample.header.time_quality)) &&
           out.append('}');
  }
  static bool write_result(const storage::ImmutableRecordBytes& record,
                           telemetry_contract::CanonicalBuffer& out,
                           storage::DecodedResultPayload& decoded) noexcept {
    if (storage::RecordCodec::decode_result(record, decoded) != storage::TypedDecodeStatus::Decoded) {
      return false;
    }
    return out.append('{') && key(out, "boot_id") &&
           hex(out, record.identity.boot_id.bytes.data(), record.identity.boot_id.bytes.size()) &&
           out.append(',') && key(out, "computed_monotonic_us") &&
           uint64_string(out, decoded.computed_monotonic_us) && out.append(',') &&
           key(out, "config_ref") && append_config_ref(out, decoded.config_ref) &&
           out.append(',') && key(out, "device_id") &&
           hex(out, record.identity.device_id.bytes.data(), record.identity.device_id.bytes.size()) &&
           out.append(',') && key(out, "fuzzy") && append_fis(out, decoded.fis) &&
           out.append(',') && key(out, "kind") && append_string(out, "result") &&
           out.append(',') && key(out, "monotonic_us") &&
           uint64_string(out, decoded.computed_monotonic_us) && out.append(',') &&
           key(out, "processed") && append_processed(out, decoded.metrics) && out.append(',') &&
           key(out, "profile_refs") &&
           append_profile_refs(out, decoded.profile_refs.data(), decoded.profile_ref_count) &&
           out.append(',') && key(out, "record_id") && append_record_id(out, record.identity) &&
           out.append(',') && key(out, "recorded_at") &&
           append_recorded_at(out, acquisition::RecordedAt{
               decoded.recorded_at_available, decoded.recorded_at_epoch_ms}) && out.append(',') &&
           key(out, "safety") && append_safety(out, decoded) && out.append(',') &&
           key(out, "sequence") && uint64_string(out, record.identity.sequence) && out.append(',') &&
           key(out, "source_sample_ids") &&
           append_source_ids(out, decoded.source_sample_ids.data(), decoded.source_sample_id_count) &&
           out.append(',') && key(out, "stream") &&
           append_string(out, result_stream_name(decoded.stream)) && out.append(',') &&
           key(out, "time_quality") &&
           append_string(out, decoded_time_quality_name(decoded.time_quality)) && out.append('}');
  }
  static bool append_source_ids(telemetry_contract::CanonicalBuffer& out,
                                const storage::DecodedSourceId* sources,
                                std::size_t count) noexcept {
    if (count == 0U || !out.append('[')) return false;
    for (std::size_t index = 0U; index < count; ++index) {
      if (index != 0U && !out.append(',')) return false;
      if (!out.append('"') ||
          !append_hex_unquoted(out, sources[index].device_id.bytes.data(), sources[index].device_id.bytes.size()) ||
          !out.append('/') ||
          !append_hex_unquoted(out, sources[index].boot_id.bytes.data(), sources[index].boot_id.bytes.size()) ||
          !out.append('/') ||
          !telemetry_contract::detail::append_uint64(out, sources[index].sequence) ||
          !out.append('"')) return false;
    }
    return out.append(']');
  }
  static const char* deployment_name(uint8_t deployment) noexcept {
    switch (deployment) {
      case 1U: return "TEST_ONLY";
      case 2U: return "PRODUCTION";
      default: return "UNCONFIGURED";
    }
  }
  static const char* fis_state_name(uint8_t state) noexcept {
    switch (state) {
      case 0U: return "UNCONFIGURED";
      case 2U: return "READY";
      default: return "UNAVAILABLE";
    }
  }
  static const char* fis_reason_name(uint8_t reason) noexcept {
    switch (reason) {
      case 0U: return "MODEL_NOT_CONFIGURED";
      case 1U: return "MODEL_INVALID";
      case 2U: return "MODEL_INVALID";
      case 3U: return "INPUT_MISSING";
      case 4U: return "STALE_INPUT";
      case 5U: return "INPUT_CALIBRATING";
      case 6U: return "INPUT_MISSING";
      case 7U: return "INPUT_MISSING";
      case 8U: return "INPUT_MISSING";
      case 9U: return "ZERO_AREA";
      default: return "READY";
    }
  }
  static const char* safety_state_name(uint8_t state) noexcept {
    switch (state) {
      case 0U: return "UNCONFIGURED";
      case 1U: return "UNAVAILABLE";
      case 2U: return "FAULT";
      case 3U: return "CLEAR";
      case 4U: return "ALERT";
      default: return "FAULT";
    }
  }
  static const char* safety_reason_name(uint8_t reason) noexcept {
    switch (reason) {
      case 0U: return "SAFETY_NOT_CONFIGURED";
      case 1U: return "INVALID_MODEL";
      case 2U: return "HAZARD_INPUT_MISSING";
      case 3U: return "HAZARD_INPUT_STALE";
      case 4U: return "HAZARD_INPUT_CALIBRATING";
      case 5U: return "HAZARD_INPUT_UNCALIBRATED";
      case 6U: return "HAZARD_INPUT_TIMEOUT";
      case 7U: return "HAZARD_INPUT_INVALID";
      case 8U: return "HAZARD_THRESHOLD_EXCEEDED";
      case 9U: return "ALERT_LATCHED";
      default: return "ALL_VALIDATED_CHANNELS_CLEAR";
    }
  }
  static const char* metric_name(std::size_t index) noexcept {
    switch (index) {
      case 0U: return "fill_pct";
      case 1U: return "weight_kg";
      case 2U: return "weight_utilization_pct";
      case 3U: return "ogi";
      case 4U: return "accumulation_hours";
      default: return "temperature_c";
    }
  }
  static const char* fis_input_name(uint8_t input) noexcept {
    switch (input) {
      case 1U: return "fill_pct";
      case 2U: return "weight_utilization_pct";
      case 3U: return "temperature_c";
      case 4U: return "mq7_mv";
      case 5U: return "mq8_mv";
      default: return "unknown";
    }
  }
  static bool append_metric(telemetry_contract::CanonicalBuffer& out,
                            const storage::DecodedMetric& metric) noexcept {
    return metric.available ? append_float(out, metric.value) : out.append("null");
  }
  static bool append_processed(telemetry_contract::CanonicalBuffer& out,
                               const std::array<storage::DecodedMetric, 8U>& metrics) noexcept {
    if (!out.append('{')) return false;
    constexpr std::array<std::size_t, 6U> order{{4U, 0U, 3U, 5U, 1U, 2U}};
    for (std::size_t index = 0U; index < 6U; ++index) {
      if (index != 0U && !out.append(',')) return false;
      const std::size_t metric_index = order[index];
      if (!key(out, metric_name(metric_index)) ||
          !append_metric(out, metrics[metric_index])) return false;
    }
    return out.append('}');
  }
  static bool append_model_ref(telemetry_contract::CanonicalBuffer& out,
                               uint32_t version,
                               const config::ConfigHash& hash) noexcept {
    return out.append('{') && key(out, "hash") && prefixed_hash(out, hash) &&
           out.append(',') && key(out, "version") && uint64_string(out, version) && out.append('}');
  }
  static bool append_fis_trace(telemetry_contract::CanonicalBuffer& out,
                               const storage::DecodedFisTrace& trace) noexcept {
    if (!out.append("{\"aggregate\":")) return false;
    if (!trace.aggregate_present) {
      if (!out.append("null")) return false;
    } else {
      if (!out.append('{') || !key(out, "area") || !append_float(out, trace.aggregate_area) ||
          !out.append(',') || !key(out, "centroid")) return false;
      if (trace.centroid_present ? !append_float(out, trace.aggregate_centroid)
                                 : !out.append("null")) return false;
      if (!out.append(',') || !key(out, "output_id") ||
          !append_model_id(out, trace.aggregate_output_id.data(), trace.aggregate_output_id.size()) ||
          !out.append('}')) return false;
    }
    if (!out.append(",\"memberships\":[")) return false;
    for (std::size_t index = 0U; index < trace.membership_count; ++index) {
      const auto& membership = trace.memberships[index];
      if (index != 0U && !out.append(',')) return false;
      if (!out.append('{') || !key(out, "input") || !append_string(out, fis_input_name(membership.input)) ||
          !out.append(',') || !key(out, "set") || !append_model_id(out, membership.set_id.data(), membership.set_id.size()) ||
          !out.append(',') || !key(out, "value") || !append_float(out, membership.value) || !out.append('}')) return false;
    }
    if (!out.append("],\"model_ref\":" ) || !append_model_ref(out, trace.model_version, trace.model_hash) ||
        !out.append(",\"rules\":[")) return false;
    for (std::size_t index = 0U; index < trace.rule_count; ++index) {
      const auto& rule = trace.rules[index];
      if (index != 0U && !out.append(',')) return false;
      if (!out.append('{') || !key(out, "id") || !uint64_string(out, rule.id) ||
          !out.append(',') || !key(out, "output_id") || !append_model_id(out, rule.output_id.data(), rule.output_id.size()) ||
          !out.append(',') || !key(out, "strength") || !append_float(out, rule.strength) || !out.append('}')) return false;
    }
    return out.append("]}");
  }
  static bool append_fis(telemetry_contract::CanonicalBuffer& out,
                         const storage::DecodedFisTrace& fis) noexcept {
    if (!out.append('{') || !key(out, "label")) return false;
    if (fis.label_present ? !append_model_id(out, fis.label.data(), fis.label.size())
                          : !out.append("null")) return false;
    if (!out.append(',') || !key(out, "model_state") || !append_string(out, deployment_name(fis.model_state)) ||
        !out.append(',') || !key(out, "reason") || !append_string(out, fis_reason_name(fis.reason)) ||
        !out.append(',') || !key(out, "score")) return false;
    if (fis.score_present ? !append_float(out, fis.score) : !out.append("null")) return false;
    if (!out.append(',') || !key(out, "state") || !append_string(out, fis_state_name(fis.state)) ||
        !out.append(',') || !key(out, "trace")) return false;
    if (!fis.trace_present) return out.append("null}");
    return append_fis_trace(out, fis) && out.append('}');
  }
  static const char* safety_signal_name(uint8_t signal) noexcept {
    switch (signal) {
      case 1U: return "temperature_c";
      case 2U: return "mq7_mv";
      case 3U: return "mq8_mv";
      default: return "unknown";
    }
  }
  static const char* event_type_name(uint8_t kind) noexcept {
    // ProfileReset is a profile lifecycle event, not a device reboot.
    return "HEALTH_TRANSITION";
  }
  static const char* metadata_type_name(
      storage::ArchiveMetadataType type) noexcept {
    switch (type) {
      case storage::ArchiveMetadataType::ConfigActivation: return "CONFIG_ACTIVATION";
      case storage::ArchiveMetadataType::ProfileActivation: return "PROFILE_ACTIVATION";
      case storage::ArchiveMetadataType::Boot: return "BOOT";
      case storage::ArchiveMetadataType::HealthSnapshot: return "HEALTH_SNAPSHOT";
    }
    return "BOOT";
  }
  static const char* reset_reason_name(uint8_t reason) noexcept {
    switch (reason) {
      case 1U: return "POWER_ON";
      case 2U: return "BROWNOUT";
      case 3U: return "WATCHDOG";
      case 4U: return "SOFTWARE";
      default: return "UNKNOWN";
    }
  }
  static const char* decoded_time_quality_name(uint8_t quality) noexcept {
    return time_quality(static_cast<acquisition::TimeQuality>(quality));
  }
  static bool append_event_payload(telemetry_contract::CanonicalBuffer& out,
                                   uint8_t kind) noexcept {
    if (kind == 3U) {
      return out.append("{\"reset_reason\":\"SOFTWARE\"}");
    }
    return out.append("{\"component\":\"profile_manager\",\"from\":\"UNKNOWN\",\"reason\":\"PROFILE_EVENT\",\"to\":\"ONLINE\"}");
  }
  static bool append_safety_trace(telemetry_contract::CanonicalBuffer& out,
                                  const storage::DecodedSafetyTrace& trace) noexcept {
    if (!out.append("{\"aggregate\":null,\"memberships\":[")) return false;
    for (std::size_t index = 0U; index < trace.evidence_count; ++index) {
      const auto& evidence = trace.evidence[index];
      if (index != 0U && !out.append(',')) return false;
      if (!out.append('{') || !key(out, "input") || !append_string(out, safety_signal_name(evidence.signal)) ||
          !out.append(',') || !key(out, "set") || !append_string(out, evidence.threshold_exceeded ? "alert" : "clear") ||
          !out.append(',') || !key(out, "value") || !append_float(out, evidence.value) || !out.append('}')) return false;
    }
    if (!out.append("],\"model_ref\":" ) || !append_model_ref(out, trace.model_ref.profile_version, trace.model_ref.hash) ||
        !out.append(",\"rules\":[]}")) return false;
    return true;
  }
  static bool append_safety(telemetry_contract::CanonicalBuffer& out,
                            const storage::DecodedResultPayload& result) noexcept {
    if (!out.append('{') || !key(out, "model_state") || !append_string(out, deployment_name(result.safety_model_state)) ||
        !out.append(',') || !key(out, "reason") || !append_string(out, safety_reason_name(result.safety_reason)) ||
        !out.append(',') || !key(out, "state") || !append_string(out, safety_state_name(result.safety_state)) ||
        !out.append(',') || !key(out, "trace")) return false;
    if (result.safety_model_state == 0U && result.safety_trace.evidence_count == 0U) {
      return out.append("null}");
    }
    return append_safety_trace(out, result.safety_trace) && out.append('}');
  }
  static bool write_event(const storage::ImmutableRecordBytes& record,
                          telemetry_contract::CanonicalBuffer& out) noexcept {
    storage::DecodedEventPayload decoded{};
    if (storage::RecordCodec::decode_event(record, decoded) != storage::TypedDecodeStatus::Decoded) {
      return false;
    }
    std::array<storage::DecodedProfileRef, 2U> refs{};
    std::size_t ref_count{0U};
    if (decoded.previous.present) refs[ref_count++] = decoded.previous;
    if (decoded.current.present) refs[ref_count++] = decoded.current;
    return out.append('{') && key(out, "boot_id") &&
           hex(out, record.identity.boot_id.bytes.data(), record.identity.boot_id.bytes.size()) &&
           out.append(',') && key(out, "config_ref") && append_config_ref(out, decoded.config_ref) &&
           out.append(',') && key(out, "device_id") &&
           hex(out, record.identity.device_id.bytes.data(), record.identity.device_id.bytes.size()) &&
           out.append(',') && key(out, "event_type") &&
           append_string(out, event_type_name(decoded.event_kind)) && out.append(',') &&
           key(out, "kind") && append_string(out, "event") && out.append(',') &&
           key(out, "monotonic_us") && uint64_string(out, decoded.monotonic_us) && out.append(',') &&
           key(out, "payload") && append_event_payload(out, decoded.event_kind) && out.append(',') &&
           key(out, "profile_refs") && append_profile_refs(out, refs.data(), ref_count) && out.append(',') &&
           key(out, "record_id") && append_record_id(out, record.identity) && out.append(',') &&
           key(out, "recorded_at") && out.append("null,") &&
           key(out, "sequence") && uint64_string(out, record.identity.sequence) && out.append(',') &&
           key(out, "time_quality") && append_string(out, "UNSYNCED") && out.append('}');
  }
  static bool write_metadata(const storage::ImmutableRecordBytes& record,
                             telemetry_contract::CanonicalBuffer& out) noexcept {
    storage::DecodedMetadataPayload decoded{};
    if (storage::RecordCodec::decode_metadata(record, decoded) !=
        storage::TypedDecodeStatus::Decoded) {
      return false;
    }
    char record_id[96]{};
    const int record_id_size = make_id(record.identity, record_id, sizeof(record_id));
    if (record_id_size <= 0) return false;
    if (!out.append('{') || !key(out, "boot_id") ||
        !hex(out, record.identity.boot_id.bytes.data(), record.identity.boot_id.bytes.size()) ||
        !out.append(',') || !key(out, "config_ref") || append_config_ref(out, decoded.config_ref) == false ||
        !out.append(',') || !key(out, "device_id") ||
        !hex(out, record.identity.device_id.bytes.data(), record.identity.device_id.bytes.size()) ||
        !out.append(',') || !key(out, "kind") || !append_string(out, "metadata") ||
        !out.append(',') || !key(out, "metadata_type") ||
        !append_string(out, metadata_type_name(decoded.type)) ||
        !out.append(',') || !key(out, "monotonic_us") ||
        !uint64_string(out, decoded.monotonic_us) || !out.append(',') ||
        !key(out, "payload")) return false;
    if (decoded.type == storage::ArchiveMetadataType::Boot) {
      std::size_t length{0U};
      while (length < decoded.firmware_version.size() &&
             decoded.firmware_version[length] != '\0') {
        ++length;
      }
      if (length == 0U) return false;
      if (!out.append("{\"firmware_version\":" ) ||
          !append_string(out, std::string_view(decoded.firmware_version.data(), length)) ||
          !out.append(",\"reset_reason\":" ) ||
          !append_string(out, reset_reason_name(decoded.reset_reason)) ||
          !out.append('}')) {
        return false;
      }
    } else {
      return false;
    }
    return out.append(',') && key(out, "profile_refs") &&
           append_profile_refs(out, decoded.profile_refs.data(), decoded.profile_ref_count) &&
           out.append(',') && key(out, "record_id") &&
           append_string(out, std::string_view(record_id, static_cast<std::size_t>(record_id_size))) &&
           out.append(',') && key(out, "recorded_at") &&
           append_recorded_at(out, acquisition::RecordedAt{
               decoded.recorded_at_available, decoded.recorded_at_epoch_ms}) &&
           out.append(',') && key(out, "sequence") &&
           uint64_string(out, record.identity.sequence) &&
           out.append(',') && key(out, "time_quality") &&
           append_string(out, decoded_time_quality_name(decoded.time_quality)) &&
           out.append('}');
  }
  static bool insert_record_hash(telemetry_contract::CanonicalBuffer& out,
                                 const config::ConfigHash& hash) noexcept {
    const std::string_view marker{",\"recorded_at\":"};
    std::size_t position = 0U;
    for (; position + marker.size() <= out.size(); ++position) {
      if (std::memcmp(out.data() + position,
                      marker.data(), marker.size()) == 0) break;
    }
    if (position + marker.size() > out.size()) return false;
    constexpr std::string_view prefix{",\"record_hash\":\"sha256:"};
    // The inserted JSON consists of the prefix, 64 lowercase hex digits,
    // and one closing quote. Do not count a second byte: the following
    // record bytes already include the comma before recorded_at.
    const std::size_t added = prefix.size() + 64U + 1U;
    if (out.size() + added > out.capacity()) return false;
    std::memmove(out.data() + position + added,
                 out.data() + position, out.size() - position);
    std::memcpy(out.data() + position, prefix.data(), prefix.size());
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0U; index < hash.size(); ++index) {
      out.data()[position + prefix.size() + index * 2U] = digits[hash[index] >> 4U];
      out.data()[position + prefix.size() + index * 2U + 1U] = digits[hash[index] & 0x0fU];
    }
    out.data()[position + prefix.size() + 64U] = '"';
    return out.set_size(out.size() + added);
  }
  static int make_id(const storage::ArchiveIdentity& identity,
                     char* output, std::size_t capacity) noexcept {
    if (output == nullptr || capacity < 4U || !identity.device_id.present ||
        !identity.boot_id.present) return 0;
    constexpr char digits[] = "0123456789abcdef";
    std::size_t offset{0U};
    const auto append_id = [&](const auto& id) noexcept -> bool {
      for (const uint8_t byte : id.bytes) {
        if (offset + 2U >= capacity) return false;
        output[offset++] = digits[byte >> 4U];
        output[offset++] = digits[byte & 0x0fU];
      }
      return true;
    };
    if (!append_id(identity.device_id) || offset + 1U >= capacity) return 0;
    output[offset++] = '/';
    if (!append_id(identity.boot_id) || offset + 8U >= capacity) return 0;
    output[offset++] = '/';
    char sequence[32]{};
    const int written_sequence = std::snprintf(
        sequence, sizeof(sequence), "%llu",
        static_cast<unsigned long long>(identity.sequence));
    if (written_sequence <= 0 || offset + static_cast<std::size_t>(written_sequence) >= capacity) return 0;
    std::memcpy(output + offset, sequence, static_cast<std::size_t>(written_sequence));
    offset += static_cast<std::size_t>(written_sequence);
    output[offset] = '\0';
    const int written = static_cast<int>(offset);
    return written > 0 && static_cast<std::size_t>(written) < capacity ? written : 0;
  }
  static int make_batch_id(const storage::ArchiveIdentity& identity,
                           char* output, std::size_t capacity) noexcept {
    const int record_size = make_id(identity, output, capacity);
    if (record_size <= 0) return 0;
    constexpr std::string_view suffix{"/batch"};
    if (static_cast<std::size_t>(record_size) + suffix.size() + 1U >= capacity) return 0;
    std::memcpy(output + record_size, suffix.data(), suffix.size());
    output[record_size + suffix.size()] = '\0';
    return record_size + static_cast<int>(suffix.size());
  }

};

template <typename BatchStore, typename AckJournal>
class ArchiveBatchRuntime final {
 public:
  explicit ArchiveBatchRuntime(storage::IArchiveStore* archive = nullptr,
                      BatchStore* batches = nullptr,
                      AckJournal* journal = nullptr) noexcept
      : archive_(archive), batches_(batches), journal_(journal) {}

  void bind(storage::IArchiveStore* archive, BatchStore* batches,
            AckJournal* journal) noexcept {
    archive_ = archive;
    batches_ = batches;
    journal_ = journal;
  }

  void bind_debug(debug::IEventSink* sink,
                  acquisition::InjectedMonotonicClock clock) noexcept {
    debug_sink_ = sink;
    debug_clock_ = clock;
  }

  BatchRuntimeStatus poll() noexcept {
    if (archive_ == nullptr || batches_ == nullptr || journal_ == nullptr) {
      return BatchRuntimeStatus::Retryable;
    }
    if (!archive_->peek_pending(source_)) return BatchRuntimeStatus::Idle;
    if (!builder_.build(source_, batch_)) return BatchRuntimeStatus::Invalid;
    const auto batch_status = batches_->append(batch_);
    if (batch_status == BatchStoreStatus::Retryable) return BatchRuntimeStatus::Retryable;
    if (batch_status == BatchStoreStatus::LoggingBlocked) return BatchRuntimeStatus::LoggingBlocked;
    if (batch_status != BatchStoreStatus::Durable) return BatchRuntimeStatus::Invalid;
    if (!batches_->checkpoint()) return BatchRuntimeStatus::Retryable;
    if (journal_->append_pending(batch_) != AckPersistStatus::Durable) {
      return BatchRuntimeStatus::Retryable;
    }
    pending_ = true;
    return BatchRuntimeStatus::Durable;
  }

  AckPersistStatus apply_ack(const Ack& ack) noexcept {
    if (archive_ == nullptr || journal_ == nullptr || batches_ == nullptr) {
      return AckPersistStatus::Retryable;
    }
    const auto status = journal_->apply_ack(ack);
    if (status != AckPersistStatus::Durable) return status;
    if (!journal_->checkpoint()) return AckPersistStatus::Retryable;
    // Exact stored bytes and their source binding are authoritative after a
    // restart. Re-serializing the archive record here would make an old ACK
    // depend on the current firmware serializer.
    if (!batches_->load_manifest(ack, batch_) || !ack_matches(batch_, ack) ||
        !batch_.source.present) {
      return AckPersistStatus::Retryable;
    }
    const storage::ArchiveIdentity bound_source{
        batch_.source.device_id, batch_.source.boot_id, batch_.source.sequence,
        static_cast<storage::ArchiveRecordKind>(batch_.source.record_kind)};
    emit_storage_outcome(1, ack.record_count);
    if (archive_ != nullptr) {
      const bool archive_reclaimed =
          archive_->reclaim_pending(bound_source, batch_.source.record_hash);
      if (archive_reclaimed) emit_storage_outcome(2, 1U);
      if (!archive_reclaimed &&
          archive_->contains(bound_source, batch_.source.record_hash)) {
        return AckPersistStatus::Retryable;
      }
    }
    pending_ = false;
    const auto reclaimed = journal_->reclaim(ack);
    if (reclaimed == AckPersistStatus::Durable ||
        reclaimed == AckPersistStatus::NotFound) {
      return AckPersistStatus::Durable;
    }
    return reclaimed;
  }

  bool pending() const noexcept { return pending_; }
  const ImmutableBatch& batch() const noexcept { return batch_; }

 private:
  void emit_storage_outcome(int32_t result, uint32_t count) noexcept {
    if (debug_sink_ == nullptr) return;
    debug::EventInput event{};
    event.monotonic_us = debug_clock_.now_us();
    event.module = debug::Module::Storage;
    event.event_code = 32U;
    event.arg0 = result;
    event.arg1 = static_cast<int32_t>(count);
    (void)debug_sink_->emit(event);
  }

  storage::IArchiveStore* archive_{nullptr};
  debug::IEventSink* debug_sink_{nullptr};
  acquisition::InjectedMonotonicClock debug_clock_{nullptr, nullptr};
  BatchStore* batches_{nullptr};
  AckJournal* journal_{nullptr};
  BatchJsonBuilder builder_{};
  storage::ImmutableRecordBytes source_{};
  ImmutableBatch batch_{};
  bool pending_{false};
};

}  // namespace sigap::telemetry_archive
