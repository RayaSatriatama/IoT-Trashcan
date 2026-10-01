#pragma once

#include <algorithm>

#include "acquisition/record.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

enum class UltrasonicEchoStatus : uint8_t { Pending, Ready, Timeout, OutOfRange, Disconnected };
struct UltrasonicEchoResult {
  UltrasonicEchoStatus status{UltrasonicEchoStatus::Pending};
  uint32_t echo_us{0};
};

class IUltrasonicReader {
 public:
  virtual ~IUltrasonicReader() = default;
  virtual bool trigger(uint64_t now_us) noexcept = 0;
  virtual UltrasonicEchoResult poll(uint64_t now_us) noexcept = 0;
};

class UltrasonicTriggerGate final {
 public:
  bool can_trigger(uint64_t now_us) const noexcept {
    return !has_trigger_ || now_us - last_trigger_us_ >= 60000U;
  }
  void note_trigger(uint64_t now_us) noexcept {
    last_trigger_us_ = now_us;
    has_trigger_ = true;
  }

 private:
  uint64_t last_trigger_us_{0};
  bool has_trigger_{false};
};

class UltrasonicAcquirer final {
 public:
  static constexpr uint64_t kEngineeringInterSensorGapUs{60000};
  UltrasonicAcquirer(StreamId stream, IUltrasonicReader& source, RecordFactory& factory,
                     const UltrasonicProfile* profile, uint64_t timeout_us) noexcept
      : stream_(stream), source_(source), factory_(factory), profile_(profile), timeout_us_(timeout_us) {}

  UltrasonicAcquirer(StreamId stream, IUltrasonicReader& source, RecordFactory& factory,
                     const UltrasonicProfile* profile, uint64_t timeout_us,
                     UltrasonicTriggerGate& gate) noexcept
      : stream_(stream), source_(source), factory_(factory), profile_(profile),
        timeout_us_(timeout_us), gate_(&gate) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    if (!active_) {
      if (gate_ != nullptr && !gate_->can_trigger(now_us)) return DriverPollStatus::NoAttempt;
      if (gate_ == nullptr && has_trigger_ && now_us - last_trigger_us_ < kEngineeringInterSensorGapUs) return DriverPollStatus::NoAttempt;
      if (!source_.trigger(now_us)) return DriverPollStatus::Fault;
      active_ = true;
      has_trigger_ = true;
      last_trigger_us_ = now_us;
      if (gate_ != nullptr) gate_->note_trigger(now_us);
      return DriverPollStatus::NoAttempt;
    }
    UltrasonicEchoResult result = source_.poll(now_us);
    if (result.status == UltrasonicEchoStatus::Pending && now_us - last_trigger_us_ < timeout_us_) return DriverPollStatus::NoAttempt;
    RawObservation raw{};
    ConvertedObservation converted{};
    SampleQuality quality{SampleQuality::Timeout};
    SampleReason reason{SampleReason::Timeout};
    const SensorId sensor = stream_ == StreamId::Inorganic
                                ? SensorId::UltrasonicInorganic
                                : (stream_ == StreamId::B3 ? SensorId::UltrasonicB3
                                                           : SensorId::UltrasonicOrganic);
    if (result.status == UltrasonicEchoStatus::Ready) {
      raw.present = true;
      raw.distance_us = result.echo_us;
      if (profile_ != nullptr && !profile_matches(profile_->manifest, sensor)) {
        quality = SampleQuality::Uncalibrated;
        reason = SampleReason::InvalidProfile;
      } else if (profile_ != nullptr && profile_->valid_for_conversion()) {
        const float distance_mm = static_cast<float>(result.echo_us) * profile_->echo_mm_per_us;
        const float minimum_allowed = profile_->d_full_mm - profile_->overshoot_tolerance_mm;
        const float maximum_allowed = profile_->d_empty_mm + profile_->overshoot_tolerance_mm;
        if (distance_mm < minimum_allowed || distance_mm > maximum_allowed) {
          quality = SampleQuality::OutOfRange;
          reason = SampleReason::OutOfRange;
        } else {
          quality = SampleQuality::Ready;
          reason = SampleReason::DataReady;
          converted.primary_available = true;
          converted.primary_kind = ConvertedKind::FillPercent;
          const float ratio = (profile_->d_empty_mm - distance_mm) /
                              (profile_->d_empty_mm - profile_->d_full_mm);
          converted.primary_value = std::clamp(ratio * 100.0F, 0.0F, 100.0F);
        }
      } else {
        quality = SampleQuality::Uncalibrated;
        reason = SampleReason::InvalidProfile;
      }
    } else if (result.status == UltrasonicEchoStatus::OutOfRange) {
      quality = SampleQuality::OutOfRange;
      reason = SampleReason::OutOfRange;
    } else if (result.status == UltrasonicEchoStatus::Disconnected) {
      quality = SampleQuality::Disconnected;
      reason = SampleReason::Disconnected;
    }
    active_ = false;
    return factory_.make(now_us, sensor, stream_,
                         SampleKind::UltrasonicEcho, quality, reason, raw, converted,
                         profile_ == nullptr ? ProfileRef{} : profile_->ref(), out);
  }

 private:
  StreamId stream_;
  IUltrasonicReader& source_;
  RecordFactory& factory_;
  const UltrasonicProfile* profile_;
  uint64_t timeout_us_;
  uint64_t last_trigger_us_{0};
  bool has_trigger_{false};
  bool active_{false};
  UltrasonicTriggerGate* gate_{nullptr};
};

}  // namespace sigap::acquisition
