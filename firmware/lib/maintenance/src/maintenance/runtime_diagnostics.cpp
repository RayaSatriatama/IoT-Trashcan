#include "maintenance/runtime.h"

#include <charconv>
#include <cstdint>

namespace sigap::maintenance {
namespace {

const char* stream_name(acquisition::StreamId stream) noexcept {
  switch (stream) {
    case acquisition::StreamId::Organic: return "organic";
    case acquisition::StreamId::Inorganic: return "inorganic";
    case acquisition::StreamId::B3: return "b3";
    case acquisition::StreamId::Ambient: return "ambient";
  }
  return "unknown";
}

const char* stream_name(calibration::CalibrationTarget target) noexcept {
  switch (target) {
    case calibration::CalibrationTarget::Hx711Organic:
    case calibration::CalibrationTarget::UltrasonicOrganic:
    case calibration::CalibrationTarget::Mq4:
    case calibration::CalibrationTarget::Mq135:
    case calibration::CalibrationTarget::Sht31_44:
      return "organic";
    case calibration::CalibrationTarget::Hx711Inorganic:
    case calibration::CalibrationTarget::UltrasonicInorganic:
      return "inorganic";
    case calibration::CalibrationTarget::Hx711B3:
    case calibration::CalibrationTarget::UltrasonicB3:
    case calibration::CalibrationTarget::Mq7:
    case calibration::CalibrationTarget::Mq8:
    case calibration::CalibrationTarget::Ds18b20B3:
    case calibration::CalibrationTarget::Sht31_45:
      return "b3";
  }
  return "unknown";
}

const char* quality_name(acquisition::SampleQuality quality) noexcept {
  switch (quality) {
    case acquisition::SampleQuality::Ready: return "READY";
    case acquisition::SampleQuality::WarmingUp: return "WARMING_UP";
    case acquisition::SampleQuality::HeaterPhase: return "HEATER_PHASE";
    case acquisition::SampleQuality::Uncalibrated: return "UNCALIBRATED";
    case acquisition::SampleQuality::Calibrating: return "CALIBRATING";
    case acquisition::SampleQuality::Timeout: return "TIMEOUT";
    case acquisition::SampleQuality::Disconnected: return "DISCONNECTED";
    case acquisition::SampleQuality::CrcError: return "CRC_ERROR";
    case acquisition::SampleQuality::Stale: return "STALE";
    case acquisition::SampleQuality::OutOfRange: return "OUT_OF_RANGE";
  }
  return "UNAVAILABLE";
}

const char* reason_name(acquisition::SampleReason reason) noexcept {
  switch (reason) {
    case acquisition::SampleReason::None: return "NONE";
    case acquisition::SampleReason::DataReady: return "DATA_READY";
    case acquisition::SampleReason::NotReady: return "NOT_READY";
    case acquisition::SampleReason::Timeout: return "TIMEOUT";
    case acquisition::SampleReason::Disconnected: return "DISCONNECTED";
    case acquisition::SampleReason::CrcError: return "CRC_ERROR";
    case acquisition::SampleReason::InvalidProfile: return "INVALID_PROFILE";
    case acquisition::SampleReason::HeaterPhase: return "HEATER_PHASE";
    case acquisition::SampleReason::ConversionPending: return "CONVERSION_PENDING";
    case acquisition::SampleReason::OutOfRange: return "OUT_OF_RANGE";
    case acquisition::SampleReason::ProfileMissing: return "PROFILE_MISSING";
  }
  return "NO_EVIDENCE";
}

const char* calibration_state_name(calibration::CalibrationState state) noexcept {
  switch (state) {
    case calibration::CalibrationState::Idle: return "IDLE";
    case calibration::CalibrationState::Configuring: return "CONFIGURING";
    case calibration::CalibrationState::Capture: return "CAPTURE";
    case calibration::CalibrationState::Validate: return "VALIDATE";
    case calibration::CalibrationState::Preview: return "PREVIEW";
    case calibration::CalibrationState::Commit: return "COMMIT";
    case calibration::CalibrationState::Failed: return "FAILED";
  }
  return "FAILED";
}

bool is_mq_target(calibration::CalibrationTarget target) noexcept {
  return target >= calibration::CalibrationTarget::Mq4 &&
         target <= calibration::CalibrationTarget::Mq8;
}

bool freshness_limit_us(calibration::CalibrationTarget target,
                        const acquisition::ProfileManager* profiles,
                        uint64_t& limit_us) noexcept {
  if (target != calibration::CalibrationTarget::Mq7) {
    limit_us = is_mq_target(target) ? 10000000ULL : 5000000ULL;
    return true;
  }
  if (profiles == nullptr) return false;
  acquisition::ActiveProfileSnapshot snapshot{};
  const acquisition::ProfileKey heater_key{
      acquisition::ProfileKind::Heater, acquisition::ProfileTarget::Mq7};
  if (!profiles->snapshot(heater_key, snapshot) || !snapshot.present) return false;
  acquisition::Mq7HeaterProfile heater{};
  acquisition::ProfileCodec codec{};
  if (!codec.decode(snapshot.encoded, heater) || !heater.valid() ||
      heater.high_clean_us > UINT64_MAX - heater.low_detect_us) {
    return false;
  }
  limit_us = heater.high_clean_us + heater.low_detect_us;
  return true;
}

bool append_flag(MaintenanceResponse& response, const char* name,
                 bool value, bool separator = true) noexcept {
  return (!separator || response.append(",")) && response.append(name) &&
         response.append(value ? "1" : "0");
}

bool append_protocol_flags(MaintenanceResponse& response,
                           const calibration::DiagnosticEvidence& evidence) noexcept {
  if (!response.append("f=")) return false;
  switch (evidence.sensor) {
    case acquisition::SensorId::Mq4:
    case acquisition::SensorId::Mq135:
    case acquisition::SensorId::Mq7:
    case acquisition::SensorId::Mq8:
      return append_flag(response, "id=", evidence.identity_valid, false) &&
             append_flag(response, "heat=", evidence.heater_ready);
    case acquisition::SensorId::Ds18b20B3:
      return append_flag(response, "id=", evidence.identity_valid, false) &&
             append_flag(response, "crc=", evidence.crc_valid) &&
             append_flag(response, "conv=", evidence.conversion_complete) &&
             append_flag(response, "wire=", evidence.wire_evidence);
    case acquisition::SensorId::Sht31_44:
    case acquisition::SensorId::Sht31_45:
      return append_flag(response, "id=", evidence.identity_valid, false) &&
             append_flag(response, "tc=", evidence.temperature_crc_valid) &&
             append_flag(response, "hc=", evidence.humidity_crc_valid);
    default:
      return append_flag(response, "id=", evidence.identity_valid, false);
  }
}

}  // namespace

CommandStatus CommandRouter::sensor_status(const MaintenanceCommand& command,
                                           uint64_t now_us,
                                           MaintenanceResponse& response) noexcept {
  calibration::CalibrationTarget target{};
  if (!calibration_target_from_text(command.target, target)) {
    return CommandStatus::InvalidSyntax;
  }
  calibration::DiagnosticEvidence evidence{};
  const bool available = evidence_ != nullptr && evidence_->diagnostic(target, evidence);
  const uint64_t age_us = !available || now_us < evidence.observed_at_us
                              ? 0U : now_us - evidence.observed_at_us;
  uint64_t freshness_limit{0U};
  const bool freshness_available = freshness_limit_us(target, profiles_, freshness_limit);
  const bool fresh = available && freshness_available &&
                     evidence.quality != acquisition::SampleQuality::Stale &&
                     age_us <= freshness_limit;
  acquisition::ActiveProfileSnapshot profile{};
  bool profile_present{false};
  if (profiles_ != nullptr) {
    const auto profile_target = static_cast<acquisition::ProfileTarget>(
        static_cast<uint8_t>(target) + 1U);
    profile_present = profiles_->snapshot(
        acquisition::default_profile_key(profile_target), profile) && profile.present;
  }
  if (!response.append("s=") || !response.append(command.target.view()) ||
      !response.append("\nst=") ||
      !response.append(available ? stream_name(evidence.stream) : stream_name(target)) ||
      !response.append("\nav=") || !response.append(available ? "1\n" : "0\n") ||
      !response.append("q=") ||
      !response.append(available ? quality_name(evidence.quality) : "UNAVAILABLE") ||
      !response.append("\nr=") ||
      !response.append(available ? reason_name(evidence.reason) : "NO_EVIDENCE") ||
      !response.append("\nraw=") ||
      !response.append(available && evidence.raw_present ? "1\n" : "0\n") ||
      !append_u64(response, "age=", age_us) ||
      !response.append("fr=") ||
      !response.append(!available || !freshness_available ? "UNAVAILABLE\n" :
                       fresh ? "FRESH\n" : "STALE\n") ||
      !(available ? append_protocol_flags(response, evidence) : response.append("f=na")) ||
      !response.append("\np=") || !response.append(profile_present ? "1\n" : "0\n") ||
      !append_u64(response, "pv=", profile_present ? profile.ref.profile_version : 0U) ||
      !response.append("c=") || !response.append(calibration_state_name(
          calibration_.target() == target ? calibration_.state()
                                          : calibration::CalibrationState::Idle)) ||
      !response.append("\n") ||
      !append_u64(response, "a=", available ? evidence.accepted_count : 0U) ||
      !append_u64(response, "x=", available ? evidence.rejected_count : 0U)) {
    return CommandStatus::Failed;
  }
  return CommandStatus::Accepted;
}

}  // namespace sigap::maintenance
