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
                RecordFactory& factory, const Hx711Profile* profile) noexcept
      : sensor_(sensor), stream_(stream), source_(source), factory_(factory), profile_(profile) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    if (!source_.ready()) return DriverPollStatus::NoAttempt;
    const Hx711ReadResult result = source_.read();
    RawObservation raw{};
    raw.present = result.ok || result.disconnected;
    raw.signed_counts = result.counts;
    ConvertedObservation converted{};
    SampleQuality quality = SampleQuality::Timeout;
    SampleReason reason = result.reason;
    if (result.disconnected) {
      quality = SampleQuality::Disconnected;
      reason = SampleReason::Disconnected;
    } else if (!result.ok) {
      quality = SampleQuality::Timeout;
      if (reason == SampleReason::None) reason = SampleReason::Timeout;
    } else if (profile_ != nullptr &&
               !profile_matches(profile_->manifest, sensor_)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
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
      converted.primary_kind = ConvertedKind::Kilograms;
      const double net_counts = static_cast<double>(result.counts) -
                                static_cast<double>(profile_->tare_offset_counts);
      converted.primary_value = static_cast<float>(net_counts /
                                                   static_cast<double>(profile_->counts_per_kg));
    }
    return factory_.make(now_us, sensor_, stream_, SampleKind::Hx711Counts,
                         quality, reason, raw, converted,
                         profile_ == nullptr ? ProfileRef{} : profile_->ref(), out);
  }

 private:
  SensorId sensor_;
  StreamId stream_;
  IHx711Reader& source_;
  RecordFactory& factory_;
  const Hx711Profile* profile_;
};

}  // namespace sigap::acquisition
