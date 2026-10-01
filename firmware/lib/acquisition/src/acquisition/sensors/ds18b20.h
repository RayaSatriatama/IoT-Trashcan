#pragma once

#include "acquisition/record.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

struct Ds18b20ReadResult {
  bool present{false};
  bool crc_valid{false};
  bool conversion_complete{false};
  int32_t temperature_centi_c{0};
  bool resolution_matches{true};
};

enum class DsChannelState : uint8_t {
  ProfileMissing,
  ProfileInvalid,
  Ready,
  ProfileApplyFailed,
};

class IDs18b20Reader {
 public:
  virtual ~IDs18b20Reader() = default;
  // Target adapters use begin() to apply and verify the profile's physical
  // resolution. Native fakes retain the default because their seam already
  // represents a configured device.
  virtual bool begin() noexcept { return true; }
  virtual bool request_conversion() noexcept = 0;
  // The default keeps native seams source-compatible while target adapters
  // can report the actual one-wire conversion-complete state.
  virtual bool conversion_ready() noexcept { return true; }
  virtual Ds18b20ReadResult read_conversion() noexcept = 0;
};

class Ds18b20Acquirer final {
 public:
  Ds18b20Acquirer(IDs18b20Reader& source, RecordFactory& factory,
                  const Ds18b20Profile* profile, uint64_t conversion_time_us,
                  DsChannelState channel_state = DsChannelState::Ready) noexcept
      : source_(source), factory_(factory), profile_(profile),
        conversion_time_us_(conversion_time_us),
        channel_state_(resolve_channel_state(profile, channel_state)) {}

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    if (channel_state_ == DsChannelState::ProfileMissing) {
      return make_profile_record(now_us, SampleReason::ProfileMissing, out);
    }
    if (channel_state_ == DsChannelState::ProfileInvalid ||
        channel_state_ == DsChannelState::ProfileApplyFailed) {
      return make_profile_record(now_us, SampleReason::InvalidProfile, out);
    }
    if (!pending_) {
      if (!source_.request_conversion()) {
        RawObservation raw{};
        ConvertedObservation converted{};
        return factory_.make(
            now_us, SensorId::Ds18b20B3, StreamId::B3, SampleKind::Ds18b20,
            SampleQuality::Disconnected, SampleReason::Disconnected,
            raw, converted, profile_ref(), out);
      }
      pending_ = true;
      requested_at_ = now_us;
      return DriverPollStatus::NoAttempt;
    }
    if (now_us < requested_at_ ||
        now_us - requested_at_ < conversion_time_us_) {
      return DriverPollStatus::NoAttempt;
    }
    if (!source_.conversion_ready()) {
      pending_ = false;
      RawObservation raw{};
      ConvertedObservation converted{};
      return factory_.make(
          now_us, SensorId::Ds18b20B3, StreamId::B3, SampleKind::Ds18b20,
          SampleQuality::Timeout, SampleReason::ConversionPending,
          raw, converted, profile_ref(), out);
    }
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
    } else if (!result.resolution_matches) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
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
                         profile_ref(), out);
  }

 private:
  static DsChannelState resolve_channel_state(
      const Ds18b20Profile* profile,
      DsChannelState requested) noexcept {
    if (requested != DsChannelState::Ready) return requested;
    if (profile == nullptr) return DsChannelState::ProfileMissing;
    if (!profile->valid_for_acquisition() ||
        !profile_matches(profile->manifest, SensorId::Ds18b20B3)) {
      return DsChannelState::ProfileInvalid;
    }
    return DsChannelState::Ready;
  }

  DriverPollStatus make_profile_record(uint64_t now_us,
                                       SampleReason reason,
                                       SampleRecord& out) noexcept {
    RawObservation raw{};
    ConvertedObservation converted{};
    return factory_.make(
        now_us, SensorId::Ds18b20B3, StreamId::B3, SampleKind::Ds18b20,
        SampleQuality::Uncalibrated, reason, raw, converted,
        profile_ref(), out);
  }

  ProfileRef profile_ref() const noexcept {
    return profile_ != nullptr && profile_->valid_for_acquisition() &&
                   profile_matches(profile_->manifest, SensorId::Ds18b20B3)
               ? profile_->ref()
               : ProfileRef{};
  }

  IDs18b20Reader& source_;
  RecordFactory& factory_;
  const Ds18b20Profile* profile_;
  uint64_t conversion_time_us_;
  DsChannelState channel_state_;
  uint64_t requested_at_{0};
  bool pending_{false};
};

}  // namespace sigap::acquisition
