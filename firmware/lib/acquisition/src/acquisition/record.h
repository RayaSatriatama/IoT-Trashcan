#pragma once

#include "acquisition/acquisition_types.h"
#include "acquisition/sequence.h"

namespace sigap::acquisition {

class IUtcSource {
 public:
  virtual ~IUtcSource() = default;
  virtual bool now_ms(int64_t& epoch_ms) noexcept = 0;
};

struct RecordContext {
  BootId boot_id{};
  DeviceId device_id{};
  ConfigRef config_ref{};
  IUtcSource* utc_source{nullptr};
};

class RecordFactory final {
 public:
  RecordFactory(SequenceAllocator& sequence, const RecordContext& context) noexcept
      : sequence_(sequence), context_(context) {}

  DriverPollStatus make(uint64_t monotonic_us, SensorId sensor, StreamId stream,
                        SampleKind kind, SampleQuality quality, SampleReason reason,
                        const RawObservation& raw, const ConvertedObservation& converted,
                        const ProfileRef& profile, SampleRecord& out) noexcept {
    uint64_t sequence{0};
    if (!sequence_.allocate(sequence)) return DriverPollStatus::Fault;
    out = SampleRecord{};
    out.header.sensor = sensor;
    out.header.stream = stream;
    out.header.kind = kind;
    out.header.device_id = context_.device_id;
    out.header.boot_id = context_.boot_id;
    out.header.sequence = sequence;
    out.header.monotonic_us = monotonic_us;
    out.header.config_ref = context_.config_ref;
    out.header.profile_ref = profile;
    out.header.time_quality = TimeQuality::Unsynced;
    if (context_.utc_source != nullptr) {
      int64_t epoch_ms{0};
      if (context_.utc_source->now_ms(epoch_ms)) {
        out.header.recorded_at.available = true;
        out.header.recorded_at.epoch_ms = epoch_ms;
        out.header.time_quality = TimeQuality::Synchronized;
      }
    }
    out.quality = quality;
    out.reason = reason;
    out.raw = raw;
    out.converted = converted;
    return DriverPollStatus::Produced;
  }

 private:
  SequenceAllocator& sequence_;
  RecordContext context_;
};

}  // namespace sigap::acquisition
