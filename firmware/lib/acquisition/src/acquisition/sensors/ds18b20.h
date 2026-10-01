#pragma once

#include "acquisition/record.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

struct Ds18b20ReadResult {
  bool present{false};
  bool crc_valid{false};
  bool conversion_complete{false};
  int32_t temperature_centi_c{0};
};

class IDs18b20Reader {
 public:
  virtual ~IDs18b20Reader() = default;
  virtual bool request_conversion() noexcept = 0;
  virtual Ds18b20ReadResult read_conversion() noexcept = 0;
};

class Ds18b20Acquirer final {
 public:
  Ds18b20Acquirer(IDs18b20Reader& source, RecordFactory& factory,
                  const Ds18b20Profile* profile, uint64_t conversion_time_us) noexcept
      : source_(source), factory_(factory), profile_(profile), conversion_time_us_(conversion_time_us) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    if (!pending_) {
      if (!source_.request_conversion()) return DriverPollStatus::Fault;
      pending_ = true;
      requested_at_ = now_us;
      return DriverPollStatus::NoAttempt;
    }
    if (now_us - requested_at_ < conversion_time_us_) return DriverPollStatus::NoAttempt;
    const Ds18b20ReadResult result = source_.read_conversion();
    pending_ = false;
    RawObservation raw{};
    raw.present = result.present;
    raw.temperature_centi_c = result.temperature_centi_c;
    ConvertedObservation converted{};
    SampleQuality quality{SampleQuality::Timeout};
    SampleReason reason{SampleReason::Timeout};
    if (!result.present) {
      quality = SampleQuality::Disconnected;
      reason = SampleReason::Disconnected;
    } else if (!result.crc_valid) {
      quality = SampleQuality::CrcError;
      reason = SampleReason::CrcError;
    } else if (!result.conversion_complete) {
      quality = SampleQuality::Timeout;
      reason = SampleReason::ConversionPending;
    } else if (profile_ != nullptr &&
               !profile_matches(profile_->manifest, SensorId::Ds18b20B3)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (profile_ == nullptr || !profile_->valid_for_conversion()) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else {
      quality = SampleQuality::Ready;
      reason = SampleReason::DataReady;
      converted.primary_available = true;
      converted.primary_kind = ConvertedKind::Celsius;
      converted.primary_value = static_cast<float>(result.temperature_centi_c + profile_->offset_centi_c) / 100.0F;
    }
    return factory_.make(now_us, SensorId::Ds18b20B3, StreamId::B3, SampleKind::Ds18b20,
                         quality, reason, raw, converted,
                         profile_ == nullptr ? ProfileRef{} : profile_->ref(), out);
  }

 private:
  IDs18b20Reader& source_;
  RecordFactory& factory_;
  const Ds18b20Profile* profile_;
  uint64_t conversion_time_us_;
  uint64_t requested_at_{0};
  bool pending_{false};
};

}  // namespace sigap::acquisition
