#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

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
  virtual void cancel() noexcept {}
};

class UltrasonicTriggerGate final {
 public:
  explicit UltrasonicTriggerGate(
      uint32_t minimum_gap_us =
          UltrasonicEngineeringDefaults::kInterSensorGapUs) noexcept
      : minimum_gap_us_(minimum_gap_us) {}

  bool can_trigger(uint64_t now_us) const noexcept {
    return !has_trigger_ ||
           (now_us >= last_trigger_us_ &&
            now_us - last_trigger_us_ >= minimum_gap_us_);
  }
  void note_trigger(uint64_t now_us) noexcept {
    last_trigger_us_ = now_us;
    has_trigger_ = true;
  }

 private:
  uint32_t minimum_gap_us_;
  uint64_t last_trigger_us_{0};
  bool has_trigger_{false};
};

inline uint32_t effective_ultrasonic_gap(
    const std::array<const UltrasonicProfile*, 3>& profiles) noexcept {
  constexpr std::array<SensorId, 3> kSensors{
      SensorId::UltrasonicOrganic,
      SensorId::UltrasonicInorganic,
      SensorId::UltrasonicB3,
  };

  uint32_t gap = 0U;
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    const auto params = resolve_ultrasonic_params(profiles[index], kSensors[index]);
    gap = std::max(gap, params.inter_sensor_gap_us);
  }
  return gap;
}

class UltrasonicAcquirer final {
 public:
  enum class Phase : uint8_t { Idle, WaitingEcho, TriggerFailed };
  UltrasonicAcquirer(StreamId stream, IUltrasonicReader& source, RecordFactory& factory,
                     const UltrasonicProfile* profile, uint64_t timeout_us) noexcept
      : stream_(stream), source_(source), factory_(factory), profile_(profile),
        params_{timeout_us > UINT32_MAX ? UINT32_MAX
                                        : static_cast<uint32_t>(timeout_us),
                UltrasonicEngineeringDefaults::kInterSensorGapUs, false} {}

  UltrasonicAcquirer(StreamId stream, IUltrasonicReader& source, RecordFactory& factory,
                     const UltrasonicProfile* profile,
                     UltrasonicAcquisitionParams params) noexcept
      : stream_(stream), source_(source), factory_(factory), profile_(profile),
        params_(params) {}

  UltrasonicAcquirer(StreamId stream, IUltrasonicReader& source, RecordFactory& factory,
                     const UltrasonicProfile* profile, uint64_t timeout_us,
                     UltrasonicTriggerGate& gate) noexcept
      : stream_(stream), source_(source), factory_(factory), profile_(profile),
        params_{timeout_us > UINT32_MAX ? UINT32_MAX
                                        : static_cast<uint32_t>(timeout_us),
                UltrasonicEngineeringDefaults::kInterSensorGapUs, false},
        gate_(&gate) {}

  UltrasonicAcquirer(StreamId stream, IUltrasonicReader& source, RecordFactory& factory,
                     const UltrasonicProfile* profile,
                     UltrasonicAcquisitionParams params,
                     UltrasonicTriggerGate& gate) noexcept
      : stream_(stream), source_(source), factory_(factory), profile_(profile),
        params_(params), gate_(&gate) {}

  // Starts one physical measurement. The trigger cadence is owned by the
  // scheduler, while service() must be called frequently until the echo is
  // complete or the bounded timeout expires.
  DriverPollStatus request_sample(uint64_t now_us) noexcept {
    if (phase_ != Phase::Idle) return DriverPollStatus::NoAttempt;
    if (gate_ != nullptr && !gate_->can_trigger(now_us)) {
      waiting_for_gate_ = true;
      return DriverPollStatus::NoAttempt;
    }
    if (gate_ == nullptr && has_trigger_ &&
        (now_us < last_trigger_us_ ||
         now_us - last_trigger_us_ < params_.inter_sensor_gap_us)) {
      waiting_for_gate_ = true;
      return DriverPollStatus::NoAttempt;
    }
    if (!source_.trigger(now_us)) {
      trigger_us_ = now_us;
      phase_ = Phase::TriggerFailed;
      return DriverPollStatus::NoAttempt;
    }
    trigger_us_ = now_us;
    last_trigger_us_ = now_us;
    has_trigger_ = true;
    waiting_for_gate_ = false;
    phase_ = Phase::WaitingEcho;
    if (gate_ != nullptr) gate_->note_trigger(now_us);
    return DriverPollStatus::NoAttempt;
  }

  DriverPollStatus service(uint64_t now_us, SampleRecord& out) noexcept {
    if (phase_ == Phase::Idle) {
      if (!waiting_for_gate_) return DriverPollStatus::NoAttempt;
      return request_sample(now_us);
    }

    if (phase_ == Phase::TriggerFailed) {
      phase_ = Phase::Idle;
      RawObservation raw{};
      ConvertedObservation converted{};
      return factory_.make(now_us, sensor_id(), stream_, SampleKind::UltrasonicEcho,
                           SampleQuality::Disconnected, SampleReason::Disconnected,
                           raw, converted, profile_ref(), out);
    }

    const uint64_t elapsed_us = now_us >= trigger_us_ ? now_us - trigger_us_ : 0U;
    if (elapsed_us >= params_.echo_timeout_us) {
      source_.cancel();
      phase_ = Phase::Idle;
      RawObservation raw{};
      ConvertedObservation converted{};
      return factory_.make(now_us, sensor_id(), stream_,
                           SampleKind::UltrasonicEcho,
                           SampleQuality::Timeout, SampleReason::Timeout,
                           raw, converted, profile_ref(), out);
    }

    UltrasonicEchoResult result = source_.poll(now_us);
    if (result.status == UltrasonicEchoStatus::Pending) {
      return DriverPollStatus::NoAttempt;
    }
    RawObservation raw{};
    ConvertedObservation converted{};
    SampleQuality quality{SampleQuality::Timeout};
    SampleReason reason{SampleReason::Timeout};
    const SensorId sensor = sensor_id();
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
    phase_ = Phase::Idle;
    return factory_.make(now_us, sensor, stream_,
                         SampleKind::UltrasonicEcho, quality, reason, raw, converted,
                         profile_ref(), out);
  }

  // Compatibility entry point for existing scheduler seams. New composition
  // roots should call request_sample() at sample cadence and service() from
  // every cooperative loop iteration.
  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    if (phase_ == Phase::Idle) return request_sample(now_us);
    return service(now_us, out);
  }

  Phase phase() const noexcept { return phase_; }
  bool waiting_for_echo() const noexcept { return phase_ == Phase::WaitingEcho; }

 private:
  SensorId sensor_id() const noexcept {
    return stream_ == StreamId::Inorganic
               ? SensorId::UltrasonicInorganic
               : (stream_ == StreamId::B3 ? SensorId::UltrasonicB3
                                           : SensorId::UltrasonicOrganic);
  }

  ProfileRef profile_ref() const noexcept {
    return profile_ != nullptr && profile_->valid_for_acquisition() &&
                   profile_matches(profile_->manifest, sensor_id())
               ? profile_->ref()
               : ProfileRef{};
  }

  StreamId stream_;
  IUltrasonicReader& source_;
  RecordFactory& factory_;
  const UltrasonicProfile* profile_;
  UltrasonicAcquisitionParams params_{};
  uint64_t trigger_us_{0};
  uint64_t last_trigger_us_{0};
  bool has_trigger_{false};
  bool waiting_for_gate_{false};
  Phase phase_{Phase::Idle};
  UltrasonicTriggerGate* gate_{nullptr};
};

}  // namespace sigap::acquisition
