#pragma once

#include <array>

#include "acquisition/record.h"
#include "acquisition/profiles.h"

namespace sigap::acquisition {

struct Ds18b20ReadResult {
  bool present{false};
  bool crc_valid{false};
  bool conversion_complete{false};
  int32_t temperature_centi_c{0};
  bool resolution_matches{true};
  bool identity_valid{false};
  // Complete CRC-checked protocol bytes may be physical capture or generated
  // SIM input; this flag is byte availability, not hardware attestation.
  bool wire_evidence{false};
  std::array<uint8_t, 8> rom{};
  std::array<uint8_t, 9> scratchpad{};
  uint8_t resolution_bits{0U};
};

// ROM discovery is deliberately a separate operation from the normal
// profile-bound acquisition path.  A target adapter must only enter this
// mode when an explicit calibration session owns the channel; the normal
// missing-profile path must never touch the one-wire bus.
enum class Ds18b20DiscoveryStatus : uint8_t {
  Unavailable,
  Ready,
  Disconnected,
  InvalidRom,
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
  virtual bool apply_profile(const Ds18b20Profile*) noexcept { return true; }
  virtual Ds18b20DiscoveryStatus discover() noexcept {
    return Ds18b20DiscoveryStatus::Unavailable;
  }
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
                  DsChannelState channel_state = DsChannelState::Ready,
                  const ICalibrationStateProvider* calibration = nullptr) noexcept
      : source_(source), factory_(factory), profile_(profile),
        conversion_time_us_(conversion_time_us),
        channel_state_(resolve_channel_state(profile, channel_state)),
        calibration_(calibration) {}

  void rebind_profile(const Ds18b20Profile* profile,
                      DsChannelState channel_state) noexcept {
    pending_profile_ = profile;
    pending_channel_state_ = resolve_channel_state(profile, channel_state);
    profile_update_pending_ = true;
  }

  DriverPollStatus poll(uint64_t now_us, SampleRecord& out) noexcept {
    if (!pending_ && profile_update_pending_) {
      const bool profile_ready = pending_channel_state_ == DsChannelState::ProfileMissing ||
                                 pending_channel_state_ == DsChannelState::ProfileInvalid
                                     ? true
                                     : source_.apply_profile(pending_profile_);
      profile_ = pending_profile_;
      channel_state_ = profile_ready ? pending_channel_state_
                                     : DsChannelState::ProfileApplyFailed;
      discovery_ready_ = false;
      profile_update_pending_ = false;
    }
    const bool discovery_allowed =
        profile_ == nullptr && calibration_ != nullptr &&
        calibration_->is_calibrating(SensorId::Ds18b20B3);
    if (channel_state_ == DsChannelState::ProfileMissing) {
      if (!discovery_allowed) {
        return make_profile_record(now_us, SampleReason::ProfileMissing, out);
      }
      if (!discovery_ready_) {
        const Ds18b20DiscoveryStatus status = source_.discover();
        if (status != Ds18b20DiscoveryStatus::Ready) {
          const bool disconnected =
              status == Ds18b20DiscoveryStatus::Disconnected;
          return make_profile_record(
              now_us,
              disconnected ? SampleReason::Disconnected
                           : SampleReason::InvalidProfile,
              out,
              disconnected ? SampleQuality::Disconnected
                           : SampleQuality::Uncalibrated);
        }
        discovery_ready_ = true;
      }
    }
    if (channel_state_ == DsChannelState::ProfileInvalid ||
        channel_state_ == DsChannelState::ProfileApplyFailed) {
      return make_profile_record(now_us, SampleReason::InvalidProfile, out);
    }
    if (!pending_) {
      snapshot_present_ = profile_ != nullptr;
      if (snapshot_present_) {
        snapshot_ = *profile_;
        snapshot_ref_ = snapshot_.ref();
        // The graph passes the profile-derived protocol deadline. Native
        // seams may pass a shorter injected deadline to keep the same
        // non-blocking state machine fast and deterministic in tests.
        conversion_time_snapshot_ = conversion_time_us_ != 0U
                                        ? conversion_time_us_
                                        : ds18b20_conversion_time_us(
                                              snapshot_.resolution_bits);
      } else {
        snapshot_ref_ = ProfileRef{};
        conversion_time_snapshot_ = conversion_time_us_;
      }
      if (!source_.request_conversion()) {
        RawObservation raw{};
        ConvertedObservation converted{};
        return factory_.make(
            now_us, SensorId::Ds18b20B3, StreamId::B3, SampleKind::Ds18b20,
            SampleQuality::Disconnected, SampleReason::Disconnected,
            raw, converted,
            snapshot_ref_.present ? snapshot_ref_ : profile_ref(), out);
      }
      pending_ = true;
      requested_at_ = now_us;
      return DriverPollStatus::NoAttempt;
    }
    if (now_us < requested_at_ ||
        now_us - requested_at_ < conversion_time_snapshot_) {
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
    const Ds18b20Profile* active_profile = snapshot_present_ ? &snapshot_ : profile_;
    RawObservation raw{};
    raw.present = result.present;
    raw.identity_valid = result.identity_valid;
    raw.crc_valid = result.crc_valid;
    raw.conversion_complete = result.conversion_complete;
    raw.temperature_centi_c = result.temperature_centi_c;
    raw.ds_wire_evidence = result.wire_evidence;
    if (result.wire_evidence) {
      raw.ds_rom = result.rom;
      raw.ds_scratchpad = result.scratchpad;
      raw.ds_resolution_bits = result.resolution_bits;
    }
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
    } else if (!result.identity_valid) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (active_profile != nullptr && !result.resolution_matches) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (active_profile != nullptr &&
               !profile_matches(active_profile->manifest, SensorId::Ds18b20B3)) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else if (calibration_ != nullptr &&
               calibration_->is_calibrating(SensorId::Ds18b20B3)) {
      quality = SampleQuality::Calibrating;
      reason = SampleReason::InvalidProfile;
    } else if (active_profile == nullptr || !active_profile->valid_for_conversion()) {
      quality = SampleQuality::Uncalibrated;
      reason = SampleReason::InvalidProfile;
    } else {
      quality = SampleQuality::Ready;
      reason = SampleReason::DataReady;
      converted.primary_available = true;
      converted.primary_kind = ConvertedKind::Celsius;
      converted.primary_value = static_cast<float>(result.temperature_centi_c + active_profile->offset_centi_c) / 100.0F;
    }
    return factory_.make(now_us, SensorId::Ds18b20B3, StreamId::B3, SampleKind::Ds18b20,
                         quality, reason, raw, converted,
                         snapshot_ref_.present ? snapshot_ref_ : profile_ref(), out);
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
                                       SampleRecord& out,
                                       SampleQuality quality = SampleQuality::Uncalibrated) noexcept {
    RawObservation raw{};
    ConvertedObservation converted{};
    return factory_.make(
        now_us, SensorId::Ds18b20B3, StreamId::B3, SampleKind::Ds18b20,
        quality, reason, raw, converted,
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
  uint64_t conversion_time_snapshot_{0U};
  DsChannelState channel_state_;
  uint64_t requested_at_{0};
  bool pending_{false};
  Ds18b20Profile snapshot_{};
  ProfileRef snapshot_ref_{};
  bool snapshot_present_{false};
  const ICalibrationStateProvider* calibration_{nullptr};
  const Ds18b20Profile* pending_profile_{nullptr};
  DsChannelState pending_channel_state_{DsChannelState::ProfileMissing};
  bool profile_update_pending_{false};
  bool discovery_ready_{false};
};

}  // namespace sigap::acquisition
