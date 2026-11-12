#pragma once

#include "acquisition/record.h"
#include "acquisition/profiles.h"
#include "acquisition/heater.h"

namespace sigap::acquisition {

struct MqAdcReadResult {
  bool ok{false};
  bool disconnected{false};
  uint16_t counts{0};
  SampleReason reason{SampleReason::Timeout};
};

class IMqAdcReader {
 public:
  virtual ~IMqAdcReader() = default;
  virtual MqAdcReadResult read() noexcept = 0;
};

class MqAdcAcquirer final {
 public:
  MqAdcAcquirer(SensorId sensor, StreamId stream, IMqAdcReader& source,
                RecordFactory& factory, const MqAdcProfile* profile,
                const ICalibrationStateProvider* calibration = nullptr) noexcept
      : sensor_(sensor), stream_(stream), source_(source), factory_(factory),
        profile_(profile), calibration_(calibration) {}

  MqAdcAcquirer(SensorId sensor, StreamId stream, IMqAdcReader& source,
                RecordFactory& factory, const MqAdcProfile* profile,
                const Mq7HeaterController* heater,
                const ICalibrationStateProvider* calibration = nullptr) noexcept
      : sensor_(sensor), stream_(stream), source_(source), factory_(factory),
        profile_(profile), heater_(heater), calibration_(calibration) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    MqAdcProfile attempt_profile{};
    const MqAdcProfile* active_profile = nullptr;
    if (profile_ != nullptr) {
      attempt_profile = *profile_;
      active_profile = &attempt_profile;
    }
    const MqAdcReadResult result = source_.read();
    RawObservation raw{};
    // Preserve ADC counts only as raw evidence when a conversion actually
    // succeeded. A disconnected result must not masquerade as a measurement.
    raw.present = result.ok;
    raw.identity_valid = result.ok;
    raw.adc_counts = result.counts;
    ConvertedObservation converted{};
    SampleQuality quality = SampleQuality::Timeout;
    SampleReason reason = result.reason;
    if (result.disconnected) {
      quality = SampleQuality::Disconnected;
      reason = SampleReason::Disconnected;
    } else if (!result.ok) {
      if (reason == SampleReason::None) reason = SampleReason::Timeout;
      quality = SampleQuality::Timeout;
    } else if (active_profile != nullptr && !profile_matches(active_profile->manifest, sensor_)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (sensor_ == SensorId::Mq7 &&
               (heater_ == nullptr || !heater_->operational_ready())) {
      quality = SampleQuality::HeaterPhase;
      reason = SampleReason::HeaterPhase;
    } else if (calibration_ != nullptr && calibration_->is_calibrating(sensor_)) {
      quality = SampleQuality::Calibrating;
      reason = SampleReason::InvalidProfile;
    } else if (active_profile != nullptr && active_profile->manifest.validity == ProfileValidity::Calibrating) {
      quality = SampleQuality::Calibrating;
      reason = SampleReason::InvalidProfile;
    } else if (active_profile == nullptr || !active_profile->valid_for_conversion()) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else {
      quality = SampleQuality::Ready;
      reason = SampleReason::DataReady;
      converted.primary_available = true;
      converted.primary_kind = ConvertedKind::AdcMillivolts;
      const float pin_mv = static_cast<float>(result.counts) *
                           static_cast<float>(active_profile->reference_mv) /
                           static_cast<float>(active_profile->full_scale_counts);
      converted.primary_value = pin_mv * static_cast<float>(active_profile->divider_denominator) /
                                static_cast<float>(active_profile->divider_numerator);
    }
    raw.heater_ready = sensor_ != SensorId::Mq7 ||
                       (heater_ != nullptr && heater_->operational_ready());
    if (sensor_ == SensorId::Mq7 && heater_ != nullptr) {
      raw.heater_phase_available = true;
      switch (heater_->state()) {
        case Mq7HeaterState::Preheat: raw.heater_phase = Mq7HeaterPhase::Preheat; break;
        case Mq7HeaterState::HighClean: raw.heater_phase = Mq7HeaterPhase::HighClean; break;
        case Mq7HeaterState::LowDetect: raw.heater_phase = Mq7HeaterPhase::LowDetect; break;
        case Mq7HeaterState::Fault: raw.heater_phase = Mq7HeaterPhase::Fault; break;
      }
    }
    return factory_.make(now_us, sensor_, stream_, SampleKind::MqAdc,
                         quality, reason, raw, converted,
                         active_profile == nullptr ? ProfileRef{} : active_profile->ref(), out);
  }

 private:
  SensorId sensor_;
  StreamId stream_;
  IMqAdcReader& source_;
  RecordFactory& factory_;
  const MqAdcProfile* profile_;
  const Mq7HeaterController* heater_{nullptr};
  const ICalibrationStateProvider* calibration_{nullptr};
};

}  // namespace sigap::acquisition
