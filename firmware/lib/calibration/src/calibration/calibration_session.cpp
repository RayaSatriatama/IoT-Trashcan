#include "calibration/calibration_session.h"

#include <algorithm>
#include <cmath>

#include "acquisition/target_protocol.h"

namespace sigap::calibration {
namespace {

using acquisition::ProfileTarget;
using acquisition::ProfileValidity;
using acquisition::SampleQuality;
using acquisition::SensorId;

bool accepted_quality(SampleQuality quality) noexcept {
  return quality == SampleQuality::Ready;
}

bool valid_target(uint32_t target_id, ProfileTarget first,
                  ProfileTarget last) noexcept {
  const auto target = static_cast<ProfileTarget>(target_id);
  return static_cast<uint32_t>(target) >= static_cast<uint32_t>(first) &&
         static_cast<uint32_t>(target) <= static_cast<uint32_t>(last);
}

ProfileValidationResult profile_result(ProfileValidationCode code) noexcept {
  return ProfileValidationResult{code};
}

std::size_t target_index(CalibrationTarget target) noexcept {
  return static_cast<std::size_t>(target);
}

bool target_from_sensor(acquisition::SensorId sensor,
                        CalibrationTarget& target) noexcept {
  switch (sensor) {
    case SensorId::Hx711Organic: target = CalibrationTarget::Hx711Organic; return true;
    case SensorId::Hx711Inorganic: target = CalibrationTarget::Hx711Inorganic; return true;
    case SensorId::Hx711B3: target = CalibrationTarget::Hx711B3; return true;
    case SensorId::UltrasonicOrganic: target = CalibrationTarget::UltrasonicOrganic; return true;
    case SensorId::UltrasonicInorganic: target = CalibrationTarget::UltrasonicInorganic; return true;
    case SensorId::UltrasonicB3: target = CalibrationTarget::UltrasonicB3; return true;
    case SensorId::Mq4: target = CalibrationTarget::Mq4; return true;
    case SensorId::Mq135: target = CalibrationTarget::Mq135; return true;
    case SensorId::Mq7: target = CalibrationTarget::Mq7; return true;
    case SensorId::Mq8: target = CalibrationTarget::Mq8; return true;
    case SensorId::Ds18b20B3: target = CalibrationTarget::Ds18b20B3; return true;
    case SensorId::Sht31_44: target = CalibrationTarget::Sht31_44; return true;
    case SensorId::Sht31_45: target = CalibrationTarget::Sht31_45; return true;
  }
  return false;
}

}  // namespace

void CalibrationEvidenceCache::observe(
    const acquisition::SampleRecord& sample) noexcept {
  CalibrationTarget target{};
  if (!target_from_sensor(sample.header.sensor, target)) return;
  const auto index = target_index(target);
  if (index >= present_.size() ||
      (present_[index] && sample.header.sequence <= sequences_[index])) return;
  TrialObservation observation{};
  switch (sample.header.kind) {
    case acquisition::SampleKind::Hx711Counts:
      observation = Hx711Trial{sample.raw.signed_counts, sample.quality};
      break;
    case acquisition::SampleKind::UltrasonicEcho:
      observation = UltrasonicTrial{static_cast<float>(sample.raw.distance_us),
                                    sample.quality};
      break;
    case acquisition::SampleKind::MqAdc:
      observation = MqTrial{sample.raw.adc_counts, sample.quality,
                            sample.quality == acquisition::SampleQuality::Ready};
      break;
    case acquisition::SampleKind::Ds18b20:
      observation = Ds18b20Trial{sample.raw.temperature_centi_c, sample.quality,
                                 sample.raw.present, sample.header.profile_ref.present,
                                 sample.quality == acquisition::SampleQuality::Ready,
                                 sample.quality != acquisition::SampleQuality::CrcError};
      break;
    case acquisition::SampleKind::Sht31:
      observation = Sht31Trial{sample.raw.temperature_centi_c,
                               sample.raw.humidity_centi_pct, sample.quality,
                               sample.raw.present,
                               sample.quality != acquisition::SampleQuality::CrcError,
                               sample.quality != acquisition::SampleQuality::CrcError};
      break;
  }
  observations_[index] = observation;
  sequences_[index] = sample.header.sequence;
  present_[index] = true;
}

bool CalibrationEvidenceCache::latest(
    CalibrationTarget target, uint64_t after_sequence,
    TrialObservation& output, uint64_t& observed_sequence) const noexcept {
  const auto index = target_index(target);
  if (index >= present_.size() || !present_[index] ||
      sequences_[index] <= after_sequence) return false;
  output = observations_[index];
  observed_sequence = sequences_[index];
  return true;
}

void CaptureStats::observe(double value) noexcept {
  if (!std::isfinite(value) || count == UINT32_MAX) {
    return;
  }
  if (count == 0U) {
    minimum = value;
    maximum = value;
    mean = value;
    count = 1U;
    return;
  }
  minimum = std::min(minimum, value);
  maximum = std::max(maximum, value);
  const double next_count = static_cast<double>(count) + 1.0;
  mean += (value - mean) / next_count;
  ++count;
}

double CaptureStats::span() const noexcept {
  return count == 0U ? 0.0 : maximum - minimum;
}

ProfileValidationResult validate_hx711_profile(
    const acquisition::Hx711Profile& profile) noexcept {
  if (profile.manifest.validity == ProfileValidity::Missing) {
    return profile_result(ProfileValidationCode::Missing);
  }
  if (!valid_target(profile.manifest.target_id, ProfileTarget::Hx711Organic,
                    ProfileTarget::Hx711B3)) {
    return profile_result(ProfileValidationCode::InvalidIdentity);
  }
  return profile.valid_for_conversion()
             ? profile_result(ProfileValidationCode::Valid)
             : profile_result(ProfileValidationCode::InvalidRange);
}

ProfileValidationResult validate_ultrasonic_profile(
    const acquisition::UltrasonicProfile& profile) noexcept {
  if (profile.manifest.validity == ProfileValidity::Missing) {
    return profile_result(ProfileValidationCode::Missing);
  }
  if (!valid_target(profile.manifest.target_id,
                    ProfileTarget::UltrasonicOrganic,
                    ProfileTarget::UltrasonicB3)) {
    return profile_result(ProfileValidationCode::InvalidIdentity);
  }
  return profile.valid_for_conversion()
             ? profile_result(ProfileValidationCode::Valid)
             : profile_result(ProfileValidationCode::InvalidRange);
}

ProfileValidationResult validate_mq_profile(
    const acquisition::MqAdcProfile& profile, SensorId sensor) noexcept {
  if (profile.manifest.validity == ProfileValidity::Missing) {
    return profile_result(ProfileValidationCode::Missing);
  }
  if (!acquisition::profile_matches(profile.manifest, sensor)) {
    return profile_result(ProfileValidationCode::InvalidIdentity);
  }
  return profile.valid_for_conversion()
             ? profile_result(ProfileValidationCode::Valid)
             : profile_result(ProfileValidationCode::InvalidRange);
}

ProfileValidationResult validate_mq7_profile(
    const acquisition::Mq7HeaterProfile& profile) noexcept {
  if (!profile.valid()) {
    return profile_result(ProfileValidationCode::InvalidRange);
  }
  return profile.target_id == static_cast<uint32_t>(ProfileTarget::Mq7)
             ? profile_result(ProfileValidationCode::Valid)
             : profile_result(ProfileValidationCode::InvalidIdentity);
}

ProfileValidationResult validate_ds18b20_profile(
    const acquisition::Ds18b20Profile& profile) noexcept {
  if (profile.manifest.validity == ProfileValidity::Missing) {
    return profile_result(ProfileValidationCode::Missing);
  }
  if (!acquisition::profile_matches(profile.manifest, SensorId::Ds18b20B3)) {
    return profile_result(ProfileValidationCode::InvalidIdentity);
  }
  if (!profile.valid_for_acquisition() ||
      acquisition::ds18b20_crc8(profile.rom.data(), 7U) != profile.rom[7]) {
    return profile_result(ProfileValidationCode::InvalidRange);
  }
  return profile_result(ProfileValidationCode::Valid);
}

ProfileValidationResult validate_sht31_profile(
    const acquisition::Sht31Profile& profile, uint8_t address) noexcept {
  if (profile.manifest.validity == ProfileValidity::Missing) {
    return profile_result(ProfileValidationCode::Missing);
  }
  const SensorId expected = address == 0x45U ? SensorId::Sht31_45
                                            : SensorId::Sht31_44;
  if (profile.address != address ||
      !acquisition::profile_matches(profile.manifest, expected)) {
    return profile_result(ProfileValidationCode::InvalidIdentity);
  }
  return profile.valid_for_conversion()
             ? profile_result(ProfileValidationCode::Valid)
             : profile_result(ProfileValidationCode::InvalidRange);
}

bool CalibrationSession::criteria_matches_target(
    const CaptureCriteria& criteria) const noexcept {
  const bool hx = std::holds_alternative<Hx711CaptureCriteria>(criteria);
  const bool distance = std::holds_alternative<DistanceCaptureCriteria>(criteria);
  const bool mq = std::holds_alternative<MqCaptureCriteria>(criteria);
  const bool ds = std::holds_alternative<Ds18b20CaptureCriteria>(criteria);
  const bool sht = std::holds_alternative<Sht31CaptureCriteria>(criteria);
  switch (target_) {
    case CalibrationTarget::Hx711Organic:
    case CalibrationTarget::Hx711Inorganic:
    case CalibrationTarget::Hx711B3:
      return hx;
    case CalibrationTarget::UltrasonicOrganic:
    case CalibrationTarget::UltrasonicInorganic:
    case CalibrationTarget::UltrasonicB3:
      return distance;
    case CalibrationTarget::Mq4:
    case CalibrationTarget::Mq135:
    case CalibrationTarget::Mq7:
    case CalibrationTarget::Mq8:
      return mq;
    case CalibrationTarget::Ds18b20B3:
      return ds;
    case CalibrationTarget::Sht31_44:
    case CalibrationTarget::Sht31_45:
      return sht;
  }
  return false;
}

bool CalibrationSession::criteria_valid(
    const CaptureCriteria& criteria) const noexcept {
  if (!criteria_matches_target(criteria)) return false;
  return std::visit(
      [](const auto& value) noexcept {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Hx711CaptureCriteria>) {
          return value.required_samples > 0U && value.max_span_counts >= 0;
        } else if constexpr (std::is_same_v<T, DistanceCaptureCriteria>) {
          return value.required_samples > 0U && std::isfinite(value.max_span_mm) &&
                 value.max_span_mm >= 0.0F;
        } else if constexpr (std::is_same_v<T, MqCaptureCriteria>) {
          return value.required_samples > 0U;
        } else if constexpr (std::is_same_v<T, Ds18b20CaptureCriteria>) {
          return value.required_samples > 0U && value.max_span_centi_c >= 0;
        } else {
          return value.required_samples > 0U &&
                 value.max_temperature_span_centi_c >= 0 &&
                 value.max_humidity_span_centi_pct >= 0;
        }
      },
      criteria);
}

bool CalibrationSession::start(CalibrationTarget target,
                                const CaptureCriteria& criteria,
                                uint64_t now_us) noexcept {
  if (state_ != CalibrationState::Idle) {
    return false;
  }
  target_ = target;
  if (!criteria_valid(criteria)) {
    return false;
  }
  criteria_ = criteria;
  started_at_us_ = now_us;
  last_activity_us_ = now_us;
  secondary_required_ = false;
  state_ = CalibrationState::Capture;
  failure_ = CalibrationFailure::None;
  primary_ = CaptureStats{};
  secondary_ = CaptureStats{};
  validation_ = ValidationResult{};
  return true;
}

bool CalibrationSession::valid_span(double span, double maximum) const noexcept {
  return std::isfinite(span) && std::isfinite(maximum) && span <= maximum;
}

bool CalibrationSession::required_samples_reached() const noexcept {
  const bool primary_ready = std::visit(
      [this](const auto& value) noexcept {
        return primary_.count >= value.required_samples;
      },
      criteria_);
  if (!primary_ready || !secondary_required_) return primary_ready;
  return std::visit(
      [this](const auto& value) noexcept {
        return secondary_.count >= value.required_samples;
      },
      criteria_);
}

bool CalibrationSession::observe_hx711(const Hx711Trial& trial) noexcept {
  if (!accepted_quality(trial.quality)) {
    ++primary_.quality_failures;
    return false;
  }
  primary_.observe(static_cast<double>(trial.counts));
  return true;
}

bool CalibrationSession::observe_ultrasonic(
    const UltrasonicTrial& trial) noexcept {
  if (!accepted_quality(trial.quality) || !std::isfinite(trial.distance_mm) ||
      trial.distance_mm < 0.0F) {
    ++primary_.quality_failures;
    return false;
  }
  primary_.observe(static_cast<double>(trial.distance_mm));
  return true;
}

bool CalibrationSession::observe_mq(const MqTrial& trial) noexcept {
  const auto& criteria = std::get<MqCaptureCriteria>(criteria_);
  if (!accepted_quality(trial.quality) ||
      (criteria.require_low_detect && !trial.heater_ready)) {
    ++primary_.quality_failures;
    return false;
  }
  primary_.observe(static_cast<double>(trial.adc_counts));
  return true;
}

bool CalibrationSession::observe_ds18b20(
    const Ds18b20Trial& trial) noexcept {
  const auto& criteria = std::get<Ds18b20CaptureCriteria>(criteria_);
  if (!accepted_quality(trial.quality) || !trial.raw_present ||
      !trial.conversion_complete || !trial.crc_valid ||
      (criteria.require_rom_match && !trial.rom_match)) {
    ++primary_.quality_failures;
    return false;
  }
  primary_.observe(static_cast<double>(trial.temperature_centi_c));
  return true;
}

bool CalibrationSession::observe_sht31(const Sht31Trial& trial) noexcept {
  if (!accepted_quality(trial.quality) || !trial.raw_present ||
      !trial.temperature_crc_valid || !trial.humidity_crc_valid) {
    ++primary_.quality_failures;
    return false;
  }
  primary_.observe(static_cast<double>(trial.temperature_centi_c));
  secondary_.observe(static_cast<double>(trial.humidity_centi_pct));
  return true;
}

bool CalibrationSession::observe(const TrialObservation& trial) noexcept {
  if (state_ != CalibrationState::Capture) return false;
  bool accepted = std::visit(
      [this](const auto& value) noexcept {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Hx711Trial>) {
          return std::holds_alternative<Hx711CaptureCriteria>(criteria_)
                     ? observe_hx711(value)
                     : false;
        } else if constexpr (std::is_same_v<T, UltrasonicTrial>) {
          return std::holds_alternative<DistanceCaptureCriteria>(criteria_)
                     ? observe_ultrasonic(value)
                     : false;
        } else if constexpr (std::is_same_v<T, MqTrial>) {
          return std::holds_alternative<MqCaptureCriteria>(criteria_)
                     ? observe_mq(value)
                     : false;
        } else if constexpr (std::is_same_v<T, Ds18b20Trial>) {
          return std::holds_alternative<Ds18b20CaptureCriteria>(criteria_)
                     ? observe_ds18b20(value)
                     : false;
        } else {
          return std::holds_alternative<Sht31CaptureCriteria>(criteria_)
                     ? observe_sht31(value)
                     : false;
        }
      },
      trial);
  if (accepted && required_samples_reached()) state_ = CalibrationState::Validate;
  return accepted;
}

void CalibrationSession::touch(uint64_t now_us) noexcept {
  if (state_ != CalibrationState::Idle && now_us >= last_activity_us_) {
    last_activity_us_ = now_us;
  }
}

bool CalibrationSession::observe_anchor(const TrialObservation& trial,
                                        CalibrationAnchor anchor) noexcept {
  if (anchor == CalibrationAnchor::Primary ||
      anchor == CalibrationAnchor::Tare ||
      anchor == CalibrationAnchor::Empty) {
    return observe(trial);
  }
  if (state_ != CalibrationState::Capture) return false;
  const CaptureStats saved_primary = primary_;
  primary_ = secondary_;
  const bool accepted = observe(trial);
  secondary_ = primary_;
  primary_ = saved_primary;
  secondary_required_ = true;
  if (state_ == CalibrationState::Validate && !required_samples_reached()) {
    state_ = CalibrationState::Capture;
  }
  return accepted;
}

bool CalibrationSession::tick(uint64_t now_us) noexcept {
  if (state_ == CalibrationState::Idle || state_ == CalibrationState::Failed ||
      now_us < last_activity_us_) return false;
  if (now_us - last_activity_us_ <= kSessionTimeoutUs) return false;
  fail(CalibrationFailure::Timeout);
  return true;
}

bool CalibrationSession::validate() noexcept {
  if (state_ != CalibrationState::Validate) return false;
  validation_ = ValidationResult{};
  validation_.primary = primary_;
  validation_.secondary = secondary_;
  if (primary_.count == 0U) {
    validation_.code = ValidationCode::NoEvidence;
    return false;
  }
  if (secondary_required_ && secondary_.count == 0U) {
    validation_.code = ValidationCode::InsufficientSamples;
    return false;
  }
  if (!required_samples_reached()) {
    validation_.code = ValidationCode::InsufficientSamples;
    return false;
  }
  bool stable = false;
  std::visit(
      [this, &stable](const auto& value) noexcept {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Hx711CaptureCriteria>) {
          stable = valid_span(primary_.span(), static_cast<double>(value.max_span_counts)) &&
                   (!secondary_required_ || valid_span(secondary_.span(), static_cast<double>(value.max_span_counts)));
        } else if constexpr (std::is_same_v<T, DistanceCaptureCriteria>) {
          stable = valid_span(primary_.span(), value.max_span_mm) &&
                   (!secondary_required_ || valid_span(secondary_.span(), value.max_span_mm));
        } else if constexpr (std::is_same_v<T, MqCaptureCriteria>) {
          stable = valid_span(primary_.span(), value.max_span_counts);
        } else if constexpr (std::is_same_v<T, Ds18b20CaptureCriteria>) {
          stable = valid_span(primary_.span(), value.max_span_centi_c);
        } else {
          stable = valid_span(primary_.span(), value.max_temperature_span_centi_c) &&
                   valid_span(secondary_.span(), value.max_humidity_span_centi_pct);
        }
      },
      criteria_);
  validation_.code = stable ? ValidationCode::Valid : ValidationCode::Unstable;
  return stable;
}

bool CalibrationSession::preview() noexcept {
  if (state_ != CalibrationState::Validate || !validation_.valid()) return false;
  state_ = CalibrationState::Preview;
  return true;
}

bool CalibrationSession::request_commit() noexcept {
  if (state_ != CalibrationState::Preview || !validation_.valid()) return false;
  state_ = CalibrationState::Commit;
  return true;
}

bool CalibrationSession::complete_commit(bool committed) noexcept {
  if (state_ != CalibrationState::Commit) return false;
  if (committed) {
    reset();
    return true;
  }
  fail(CalibrationFailure::CommitRejected);
  return false;
}

bool CalibrationSession::cancel() noexcept {
  if (state_ == CalibrationState::Idle || state_ == CalibrationState::Commit) {
    return false;
  }
  reset();
  return true;
}

void CalibrationSession::fail(CalibrationFailure failure) noexcept {
  if (state_ != CalibrationState::Idle) {
    failure_ = failure;
    state_ = CalibrationState::Failed;
  }
}

void CalibrationSession::reset() noexcept {
  state_ = CalibrationState::Idle;
  primary_ = CaptureStats{};
  secondary_ = CaptureStats{};
  validation_ = ValidationResult{};
  failure_ = CalibrationFailure::None;
  started_at_us_ = 0U;
  last_activity_us_ = 0U;
  secondary_required_ = false;
}

}  // namespace sigap::calibration
