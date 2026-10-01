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
                RecordFactory& factory, const MqAdcProfile* profile) noexcept
      : sensor_(sensor), stream_(stream), source_(source), factory_(factory), profile_(profile) {}

  MqAdcAcquirer(SensorId sensor, StreamId stream, IMqAdcReader& source,
                RecordFactory& factory, const MqAdcProfile* profile,
                const Mq7HeaterController* heater) noexcept
      : sensor_(sensor), stream_(stream), source_(source), factory_(factory),
        profile_(profile), heater_(heater) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    const MqAdcReadResult result = source_.read();
    RawObservation raw{};
    raw.present = result.ok || result.disconnected;
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
    } else if (profile_ != nullptr && !profile_matches(profile_->manifest, sensor_)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (sensor_ == SensorId::Mq7 &&
               (heater_ == nullptr || !heater_->operational_ready())) {
      quality = SampleQuality::HeaterPhase;
      reason = SampleReason::HeaterPhase;
    } else if (profile_ != nullptr && profile_->manifest.validity == ProfileValidity::Calibrating) {
      quality = SampleQuality::Calibrating;
      reason = SampleReason::InvalidProfile;
    } else if (profile_ == nullptr || !profile_->valid_for_conversion()) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else {
      quality = SampleQuality::Ready;
      reason = SampleReason::DataReady;
      converted.primary_available = true;
      converted.primary_kind = ConvertedKind::AdcMillivolts;
      const float pin_mv = static_cast<float>(result.counts) *
                           static_cast<float>(profile_->reference_mv) /
                           static_cast<float>(profile_->full_scale_counts);
      converted.primary_value = pin_mv * static_cast<float>(profile_->divider_denominator) /
                                static_cast<float>(profile_->divider_numerator);
    }
    return factory_.make(now_us, sensor_, stream_, SampleKind::MqAdc,
                         quality, reason, raw, converted,
                         profile_ == nullptr ? ProfileRef{} : profile_->ref(), out);
  }

 private:
  SensorId sensor_;
  StreamId stream_;
  IMqAdcReader& source_;
  RecordFactory& factory_;
  const MqAdcProfile* profile_;
  const Mq7HeaterController* heater_{nullptr};
};

}  // namespace sigap::acquisition
