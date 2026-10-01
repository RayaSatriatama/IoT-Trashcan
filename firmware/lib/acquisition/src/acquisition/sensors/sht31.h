#pragma once

#include "acquisition/record.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

struct Sht31ReadResult {
  bool ok{false};
  bool crc_valid{false};
  int32_t temperature_centi_c{0};
  int32_t humidity_centi_pct{0};
  // A shared bus adapter may issue a measurement and complete it on a later
  // poll. Pending is not a failed sensor attempt and must not create a
  // timeout record.
  bool pending{false};
  bool temperature_crc_valid{false};
  bool humidity_crc_valid{false};
};

class ISht31Reader {
 public:
  virtual ~ISht31Reader() = default;
  virtual Sht31ReadResult read(uint8_t address) noexcept = 0;
};

class Sht31Acquirer final {
 public:
  Sht31Acquirer(uint8_t address, StreamId stream, ISht31Reader& source,
                RecordFactory& factory, const Sht31Profile* profile) noexcept
      : address_(address), stream_(stream), source_(source), factory_(factory), profile_(profile) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    const Sht31ReadResult result = source_.read(address_);
    if (result.pending) {
      return DriverPollStatus::NoAttempt;
    }
    RawObservation raw{};
    raw.present = result.ok;
    raw.temperature_centi_c = result.temperature_centi_c;
    raw.humidity_centi_pct = result.humidity_centi_pct;
    ConvertedObservation converted{};
    SampleQuality quality{SampleQuality::Timeout};
    SampleReason reason{SampleReason::Timeout};
    if (!result.ok) {
      quality = SampleQuality::Timeout;
      reason = SampleReason::Timeout;
    } else if (!result.crc_valid) {
      quality = SampleQuality::CrcError;
      reason = SampleReason::CrcError;
    } else if (profile_ != nullptr &&
               !profile_matches(profile_->manifest,
                                address_ == 0x45U ? SensorId::Sht31_45
                                                  : SensorId::Sht31_44)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (profile_ == nullptr || !profile_->valid_for_conversion() ||
               profile_->address != address_) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else {
      quality = SampleQuality::Ready;
      reason = SampleReason::DataReady;
      converted.primary_available = true;
      converted.primary_kind = ConvertedKind::Celsius;
      converted.primary_value = static_cast<float>(result.temperature_centi_c + profile_->temperature_offset_centi_c) / 100.0F;
      converted.secondary_available = true;
      converted.secondary_kind = ConvertedKind::HumidityPercent;
      converted.secondary_value = static_cast<float>(result.humidity_centi_pct + profile_->humidity_offset_centi_pct) / 100.0F;
    }
    const SensorId sensor = address_ == 0x45U ? SensorId::Sht31_45 : SensorId::Sht31_44;
    return factory_.make(now_us, sensor, stream_, SampleKind::Sht31,
                         quality, reason, raw, converted,
                         profile_ == nullptr ? ProfileRef{} : profile_->ref(), out);
  }

 private:
  uint8_t address_;
  StreamId stream_;
  ISht31Reader& source_;
  RecordFactory& factory_;
  const Sht31Profile* profile_;
};

}  // namespace sigap::acquisition
