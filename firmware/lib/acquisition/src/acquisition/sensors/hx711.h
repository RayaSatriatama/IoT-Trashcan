#pragma once

#include <cmath>

#include "acquisition/record.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

struct Hx711ReadResult {
  bool ok{false};
  bool disconnected{false};
  int64_t counts{0};
  uint8_t gain{0};
  uint8_t channel{0};
  SampleReason reason{SampleReason::NotReady};
};

class IHx711Reader {
 public:
  virtual ~IHx711Reader() = default;
  virtual bool ready() noexcept = 0;
  virtual Hx711ReadResult read() noexcept = 0;
};

class Hx711Acquirer final {
 public:
  Hx711Acquirer(SensorId sensor, StreamId stream, IHx711Reader& source,
                RecordFactory& factory, const Hx711Profile* profile,
                const ICalibrationStateProvider* calibration = nullptr) noexcept
      : sensor_(sensor), stream_(stream), source_(source), factory_(factory),
        profile_(profile), calibration_(calibration) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    Hx711Profile attempt_profile{};
    const Hx711Profile* active_profile = nullptr;
    if (profile_ != nullptr) {
      attempt_profile = *profile_;
      active_profile = &attempt_profile;
    }
    RawObservation raw{};
    ConvertedObservation converted{};

    // DOUT not being ready at the scheduled deadline is an ordinary sensor
    // outcome. Emit a typed attempt instead of silently retrying forever.
    if (!source_.ready()) {
      return factory_.make(now_us, sensor_, stream_, SampleKind::Hx711Counts,
                           SampleQuality::Timeout, SampleReason::NotReady,
                           raw, converted,
                           active_profile == nullptr ? ProfileRef{} : active_profile->ref(),
                           out);
    }

    const Hx711ReadResult result = source_.read();
    // A disconnected/read-failed result is diagnostic evidence only. It does
    // not contain a trustworthy raw measurement.
    raw.present = result.ok;
    raw.identity_valid = result.ok;
    raw.signed_counts = result.counts;
    SampleQuality quality = SampleQuality::Timeout;
    SampleReason reason = result.reason;
    if (result.disconnected) {
      quality = SampleQuality::Disconnected;
      reason = SampleReason::Disconnected;
    } else if (!result.ok) {
      quality = SampleQuality::Timeout;
      if (reason == SampleReason::None) reason = SampleReason::Timeout;
    } else if (active_profile != nullptr &&
               !profile_matches(active_profile->manifest, sensor_)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
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
      converted.primary_kind = ConvertedKind::Kilograms;
      const double net_counts = static_cast<double>(result.counts) -
                                static_cast<double>(active_profile->tare_offset_counts);
      converted.primary_value = static_cast<float>(net_counts /
                                                   static_cast<double>(active_profile->counts_per_kg));
    }
    return factory_.make(now_us, sensor_, stream_, SampleKind::Hx711Counts,
                         quality, reason, raw, converted,
                         active_profile == nullptr ? ProfileRef{} : active_profile->ref(), out);
  }

 private:
  SensorId sensor_;
  StreamId stream_;
  IHx711Reader& source_;
  RecordFactory& factory_;
  const Hx711Profile* profile_;
  const ICalibrationStateProvider* calibration_{nullptr};
};

}  // namespace sigap::acquisition
