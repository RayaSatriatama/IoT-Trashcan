#pragma once

#include <cstdint>

#include "acquisition/record.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

struct Sht31ReadResult {
  bool ok{false};
  bool crc_valid{false};
  int32_t temperature_centi_c{0};
  int32_t humidity_centi_pct{0};
  uint16_t temperature_ticks{0U};
  uint16_t humidity_ticks{0U};
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
                RecordFactory& factory, const Sht31Profile* profile,
                const ICalibrationStateProvider* calibration = nullptr) noexcept
      : address_(address), stream_(stream), source_(source), factory_(factory),
        profile_(profile), calibration_(calibration) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    Sht31Profile attempt_profile{};
    const Sht31Profile* active_profile = nullptr;
    if (profile_ != nullptr) {
      attempt_profile = *profile_;
      active_profile = &attempt_profile;
    }
    const Sht31ReadResult result = source_.read(address_);
    if (result.pending) {
      return DriverPollStatus::NoAttempt;
    }
    RawObservation raw{};
    raw.present = result.ok;
    raw.identity_valid = result.ok;
    raw.crc_valid = result.crc_valid;
    raw.temperature_crc_valid = result.temperature_crc_valid;
    raw.humidity_crc_valid = result.humidity_crc_valid;
    raw.temperature_centi_c = result.temperature_centi_c;
    raw.humidity_centi_pct = result.humidity_centi_pct;
    raw.sht31_ticks_available = result.ok;
    raw.sht31_temperature_ticks = result.temperature_ticks;
    raw.sht31_humidity_ticks = result.humidity_ticks;
    ConvertedObservation converted{};
    SampleQuality quality{SampleQuality::Timeout};
    SampleReason reason{SampleReason::Timeout};
    if (!result.ok) {
      quality = SampleQuality::Timeout;
      reason = SampleReason::Timeout;
    } else if (!result.crc_valid) {
      quality = SampleQuality::CrcError;
      reason = SampleReason::CrcError;
    } else if (active_profile != nullptr &&
               !profile_matches(active_profile->manifest,
                                address_ == 0x45U ? SensorId::Sht31_45
                                                  : SensorId::Sht31_44)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (calibration_ != nullptr &&
               calibration_->is_calibrating(address_ == 0x45U ? SensorId::Sht31_45
                                                              : SensorId::Sht31_44)) {
      quality = SampleQuality::Calibrating;
      reason = SampleReason::InvalidProfile;
    } else if (active_profile == nullptr || !active_profile->valid_for_conversion() ||
               active_profile->address != address_) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else {
      quality = SampleQuality::Ready;
      reason = SampleReason::DataReady;
      converted.primary_available = true;
      converted.primary_kind = ConvertedKind::Celsius;
      converted.primary_value = static_cast<float>(result.temperature_centi_c + active_profile->temperature_offset_centi_c) / 100.0F;
      converted.secondary_available = true;
      converted.secondary_kind = ConvertedKind::HumidityPercent;
      converted.secondary_value = static_cast<float>(result.humidity_centi_pct + active_profile->humidity_offset_centi_pct) / 100.0F;
    }
    const SensorId sensor = address_ == 0x45U ? SensorId::Sht31_45 : SensorId::Sht31_44;
    return factory_.make(now_us, sensor, stream_, SampleKind::Sht31,
                         quality, reason, raw, converted,
                         active_profile == nullptr ? ProfileRef{} : active_profile->ref(), out);
  }

 private:
  uint8_t address_;
  StreamId stream_;
  ISht31Reader& source_;
  RecordFactory& factory_;
  const Sht31Profile* profile_;
  const ICalibrationStateProvider* calibration_{nullptr};
};

}  // namespace sigap::acquisition
