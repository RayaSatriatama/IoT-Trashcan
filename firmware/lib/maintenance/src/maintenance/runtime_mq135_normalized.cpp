// MQG2 producer: the normalized (dimensionless q0) MQ-135 control result.
//
// Emitted only for a normalized session/proposal or an active payload-v2
// normalized profile. MQG1 (runtime_mq135.cpp) stays byte-identical and
// readable; when no normalized mode/profile was ever selected the legacy path
// remains legitimate. Every documented group/key is always present and missing
// quantities are null, never omitted, zero, NaN or Infinity.

#include "maintenance/runtime.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace sigap::maintenance {
namespace {

// Root keys and group members differ only by the separator: the document root
// needs a comma, group members are already inside braces.
// A separator is only needed when the previous byte is not an opening brace,
// which keeps every group canonical without per-call ordering bookkeeping.
bool key(MaintenanceResponse& out, const char* name, bool comma) noexcept {
  const auto view = out.view();
  const bool needs_comma = comma && !view.empty() && view.back() != '{';
  return (!needs_comma || out.append(",")) && out.append("\"") &&
         out.append(name) && out.append("\":");
}
bool quote(MaintenanceResponse& out, const char* value) noexcept {
  if (!out.append("\"")) return false;
  for (const unsigned char c : std::string_view(value)) {
    if (c < 32U) return false;
    if ((c == '\\' || c == '"') && !out.append("\\")) return false;
    const char ch = static_cast<char>(c);
    if (!out.append(std::string_view(&ch, 1U))) return false;
  }
  return out.append("\"");
}
bool text(MaintenanceResponse& out, const char* name, const char* value,
          bool present, bool comma) noexcept {
  if (!key(out, name, comma)) return false;
  if (!present) return out.append("null");
  return quote(out, value);
}
bool number(MaintenanceResponse& out, const char* name, double value, bool present,
            bool comma) noexcept {
  if (!key(out, name, comma)) return false;
  if (!present) return out.append("null");
  if (!std::isfinite(value)) return false;
  char buffer[32]{};
  const auto n = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return n > 0 && static_cast<std::size_t>(n) < sizeof(buffer) &&
         out.append(std::string_view(buffer, static_cast<std::size_t>(n)));
}
bool small_unsigned(MaintenanceResponse& out, const char* name, uint32_t value,
                    bool present, bool comma) noexcept {
  if (!key(out, name, comma)) return false;
  if (!present) return out.append("null");
  char buffer[16]{};
  const auto n = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return n.ec == std::errc{} &&
         out.append(std::string_view(buffer, static_cast<std::size_t>(n.ptr - buffer)));
}
// 64-bit integral values use canonical decimal strings (nullable).
bool u64_string(MaintenanceResponse& out, const char* name, uint64_t value,
                bool present, bool comma) noexcept {
  if (!key(out, name, comma)) return false;
  if (!present) return out.append("null");
  char buffer[24]{};
  const auto n = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (n.ec != std::errc{}) return false;
  if (!out.append("\"")) return false;
  return out.append(std::string_view(buffer, static_cast<std::size_t>(n.ptr - buffer))) &&
         out.append("\"");
}
bool i64_string(MaintenanceResponse& out, const char* name, int64_t value,
                bool present, bool comma) noexcept {
  if (!key(out, name, comma)) return false;
  if (!present) return out.append("null");
  char buffer[24]{};
  const auto n = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (n.ec != std::errc{}) return false;
  if (!out.append("\"")) return false;
  return out.append(std::string_view(buffer, static_cast<std::size_t>(n.ptr - buffer))) &&
         out.append("\"");
}
bool boolean(MaintenanceResponse& out, const char* name, bool value,
             bool comma) noexcept {
  return key(out, name, comma) && out.append(value ? "true" : "false");
}
bool nullable_boolean(MaintenanceResponse& out, const char* name, bool value,
                      bool present, bool comma) noexcept {
  if (!key(out, name, comma)) return false;
  if (!present) return out.append("null");
  return out.append(value ? "true" : "false");
}
bool digest(MaintenanceResponse& out, const char* name, const uint8_t* bytes,
            std::size_t size, bool present, bool sha_prefix, bool comma) noexcept {
  if (!key(out, name, comma)) return false;
  if (!present) return out.append("null");
  if (!out.append(sha_prefix ? "\"sha256:" : "\"")) return false;
  constexpr char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < size; ++i) {
    const char pair[]{digits[bytes[i] >> 4U], digits[bytes[i] & 15U]};
    if (!out.append(std::string_view(pair, 2U))) return false;
  }
  return out.append("\"");
}

// Root wrappers (comma separator) and group members (comma-free first field is
// impossible, so every group member keeps a leading comma like the root).
bool root_text(MaintenanceResponse& o, const char* n, const char* v,
               bool p = true) noexcept { return text(o, n, v, p, true); }
bool member_text(MaintenanceResponse& o, const char* n, const char* v,
                 bool p = true) noexcept { return text(o, n, v, p, true); }
bool member_number(MaintenanceResponse& o, const char* n, double v,
                   bool p) noexcept { return number(o, n, v, p, true); }
bool member_small(MaintenanceResponse& o, const char* n, uint32_t v,
                  bool p) noexcept { return small_unsigned(o, n, v, p, true); }
bool member_u64(MaintenanceResponse& o, const char* n, uint64_t v,
                bool p) noexcept { return u64_string(o, n, v, p, true); }
bool member_i64(MaintenanceResponse& o, const char* n, int64_t v,
                bool p) noexcept { return i64_string(o, n, v, p, true); }
bool member_bool(MaintenanceResponse& o, const char* n, bool v) noexcept {
  return boolean(o, n, v, true);
}
bool member_nullable_bool(MaintenanceResponse& o, const char* n, bool v,
                          bool p) noexcept { return nullable_boolean(o, n, v, p, true); }
bool member_digest(MaintenanceResponse& o, const char* n, const uint8_t* b,
                   std::size_t s, bool p, bool sha) noexcept {
  return digest(o, n, b, s, p, sha, true);
}
const char* reference_kind(acquisition::Mq135ReferenceKind kind) noexcept {
  switch (kind) {
    case acquisition::Mq135ReferenceKind::Operational: return "OPERATIONAL";
    case acquisition::Mq135ReferenceKind::CleanAirConfirmed: return "CLEAN_AIR_CONFIRMED";
    case acquisition::Mq135ReferenceKind::None: break;
  }
  return "NONE";
}
const char* preparation_method(acquisition::Mq135PreparationMethod method) noexcept {
  return method == acquisition::Mq135PreparationMethod::OperatorDeclared
             ? "OPERATOR_DECLARED"
             : "UNKNOWN";
}
const char* time_quality(acquisition::TimeQuality quality) noexcept {
  return quality == acquisition::TimeQuality::Synchronized
             ? "SYNCHRONIZED"
             : quality == acquisition::TimeQuality::Estimated ? "ESTIMATED" : "UNSYNCED";
}

const char* source_quality(acquisition::SampleQuality value) noexcept {
  using Q = acquisition::SampleQuality;
  switch (value) {
    case Q::Ready: return "READY"; case Q::Calibrating: return "CALIBRATING";
    case Q::WarmingUp: return "WARMING_UP"; case Q::HeaterPhase: return "HEATER_PHASE";
    case Q::Uncalibrated: return "UNCALIBRATED"; case Q::Stale: return "STALE";
    case Q::Timeout: return "TIMEOUT"; case Q::Disconnected: return "DISCONNECTED";
    case Q::CrcError: return "CRC_ERROR"; case Q::OutOfRange: return "OUT_OF_RANGE";
  }
  return "INVALID";
}

}  // namespace

// Builds the immutable MQG2 snapshot. `now_us` is only used to evaluate age; it
// never rewrites the source timestamps. Returns false when no normalized
// session or payload-v2 profile exists (the legacy MQG1 path then applies).
bool CommandRouter::normalized_gas_diagnostic(
    calibration::CalibrationTarget target, uint64_t now_us,
    acquisition::Mq135NormalizedDiagnostic& d) const noexcept {
  if (target != calibration::CalibrationTarget::Mq135Gas) return false;
  using namespace acquisition;
  d = Mq135NormalizedDiagnostic{};

  ActiveProfileSnapshot gas_active{}, adc_active{};
  ProfileCodec codec{};
  Mq135NormalizedProfile profile{};
  const bool have_profile =
      profiles_ != nullptr && profiles_->snapshot(kMq135GasKey, gas_active) &&
      gas_active.present;
  const bool normalized_active =
      have_profile && mq135_normalized_payload(gas_active.encoded);
  const bool profile_valid =
      normalized_active && decode_mq135_normalized_profile(gas_active.encoded, profile);
  const auto* capture = calibration_.target() == calibration::CalibrationTarget::Mq135Gas &&
                                calibration_.state() != calibration::CalibrationState::Idle &&
                                calibration_.state() != calibration::CalibrationState::Failed
                            ? calibration_.normalized_criteria()
                            : nullptr;
  // Emit MQG2 for a normalized session even before a v2 profile exists.
  if (!normalized_active && capture == nullptr) return false;
  d.present = true;

  MqAdcProfile adc{};
  const bool adc_profile_present =
      profiles_ != nullptr && profiles_->snapshot(ProfileTarget::Mq135, adc_active) &&
      adc_active.present && codec.decode(adc_active.encoded, adc) &&
      adc.valid_for_conversion();

  calibration::TrialObservation observation{};
  uint64_t sequence{};
  const bool have_source = evidence_ != nullptr &&
      evidence_->latest(calibration::CalibrationTarget::Mq135Gas, 0, observation, sequence);
  const auto* source = have_source ? std::get_if<calibration::MqTrial>(&observation) : nullptr;

  const Mq135NormalizedElectrical electrical =
      profile_valid ? profile.electrical : capture != nullptr ? capture->electrical
                                                             : Mq135NormalizedElectrical{};
  const Mq135NormalizedCriteria criteria =
      profile_valid ? profile.criteria : capture != nullptr ? capture->criteria
                                                           : Mq135NormalizedCriteria{};
  const uint64_t max_age_us = criteria.max_age_us;
  const bool circuit_present = profile_valid || capture != nullptr;

  // ---- adc group -------------------------------------------------------
  const bool raw_present = source != nullptr && source->raw_present;
  const auto& cal = source != nullptr ? source->calibrated : MqAdcCalibratedVoltage{};
  const bool calibrated_ok =
      raw_present && cal.available && cal.same_sample &&
      cal.method == MqAdcCalibrationMethod::EspAdcCalEfuseTpFit &&
      cal.failure == MqAdcCalibrationFailure::None &&
      cal.unit == MqAdcVoltageUnit::Millivolt && cal.resolution_bits == 12U &&
      cal.attenuation_db == 11U && cal.adc_unit == 1U && cal.adc_channel == 4U &&
      cal.counts == source->adc_counts && cal.counts > 0U &&
      cal.counts < kMqAdcMaxCounts12Bit && std::isfinite(cal.pin_mv) &&
      cal.pin_mv > 0.0F &&
      cal.pin_mv <= static_cast<float>(kMqAdcMaxPlausiblePinMv);
  auto failure_code = cal.failure;
  if (failure_code == MqAdcCalibrationFailure::None && !calibrated_ok) {
    failure_code = raw_present && source->adc_counts < kMqAdcMaxCounts12Bit &&
                           source->adc_counts > 0U
                       ? MqAdcCalibrationFailure::NotAttempted
                       : raw_present ? MqAdcCalibrationFailure::RawOutOfRange
                                     : MqAdcCalibrationFailure::NotAttempted;
  }
  d.raw_present = raw_present;
  d.raw_counts = raw_present ? source->adc_counts : 0U;
  d.adc_available = calibrated_ok;
  d.pin_present = calibrated_ok;
  d.pin_mv = calibrated_ok ? cal.pin_mv : 0.0F;
  d.same_sample = calibrated_ok;
  d.resolution_bits = cal.resolution_bits;
  d.attenuation_db = cal.attenuation_db;
  d.adc_unit = cal.adc_unit;
  d.adc_channel = cal.adc_channel;
  {
    const char* failure = "NOT_ATTEMPTED";
    switch (failure_code) {
      case MqAdcCalibrationFailure::None: failure = "NONE"; break;
      case MqAdcCalibrationFailure::NotAttempted: failure = "NOT_ATTEMPTED"; break;
      case MqAdcCalibrationFailure::EfuseUnsupported: failure = "EFUSE_UNSUPPORTED"; break;
      case MqAdcCalibrationFailure::CharacterizeUnsupported: failure = "CHARACTERIZE_UNSUPPORTED"; break;
      case MqAdcCalibrationFailure::ConversionFailed: failure = "CONVERSION_FAILED"; break;
      case MqAdcCalibrationFailure::RawOutOfRange: failure = "RAW_OUT_OF_RANGE"; break;
      case MqAdcCalibrationFailure::ImplausibleVoltage: failure = "IMPLAUSIBLE_VOLTAGE"; break;
    }
    std::copy_n(failure, std::min(std::char_traits<char>::length(failure),
                                  d.adc_failure.size() - 1U), d.adc_failure.begin());
  }

  // ---- electrical group ------------------------------------------------
  d.circuit_present = circuit_present;
  d.circuit_mv = electrical.circuit_mv;
  d.divider_top_ohm = electrical.divider_top_ohm;
  d.divider_bottom_ohm = electrical.divider_bottom_ohm;
  d.topology = electrical.topology;
  d.hardware_confirmed = electrical.hardware_confirmed;
  if (profile_valid) {
    d.hardware_identity = profile.hardware_identity;
  } else if (capture != nullptr) {
    d.hardware_identity = capture->hardware_identity;
  }

  // ---- source/freshness ------------------------------------------------
  const bool clock_known =
      source != nullptr && boot_id_ != nullptr && boot_id_->present &&
      source->source_boot_id.present && source->source_boot_id.bytes == boot_id_->bytes &&
      now_us >= source->observed_at_us;
  const uint64_t age = clock_known ? now_us - source->observed_at_us : 0U;
  const bool age_known = clock_known && circuit_present && max_age_us > 0U;
  const bool fresh = age_known && age <= max_age_us;
  d.source_present = source != nullptr;
  if (source != nullptr) {
    d.source_boot_id = source->source_boot_id;
    d.source_sequence = source->source_sequence;
    d.source_us = source->observed_at_us;
    std::copy_n(source_quality(source->quality),
                std::min(std::char_traits<char>::length(source_quality(source->quality)),
                         d.source_quality.size() - 1U),
                d.source_quality.begin());
  }
  d.response_us = now_us;
  d.clock_known = clock_known;
  d.age_known = age_known;
  d.fresh = fresh;
  d.max_age_us = max_age_us;

  // ---- response --------------------------------------------------------
  const bool adc_binding_ok =
      profile_valid && adc_profile_present &&
      profile.adc_hash == adc.manifest.hash &&
      profile.adc_version == adc.manifest.profile_version;
  const bool same_source_profile =
      source != nullptr && adc_profile_present && source->source_profile.present &&
      source->source_profile.kind == ProfileKind::Adc &&
      source->source_profile.target_id == 8U &&
      source->source_profile.hash == adc.manifest.hash &&
      source->source_profile.profile_version == adc.manifest.profile_version;
  const bool q_ready = calibrated_ok && circuit_present && electrical.valid() &&
                       source->raw_present && source->quality == SampleQuality::Ready &&
                       source->identity_valid && fresh && same_source_profile;
  double q{0.0};
  double ao{0.0};
  bool q_ok = false;
  if (q_ready) {
    ao = static_cast<double>(cal.pin_mv) / electrical.attenuation();
    q_ok = mq135_normalized_response_double(
        static_cast<double>(electrical.circuit_mv), ao, q);
  }
  d.q_present = q_ok;
  d.q = q_ok ? q : 0.0;
  d.ao_present = q_ok;
  d.ao_mv = q_ok ? static_cast<float>(ao) : 0.0F;
  d.baseline_valid = profile_valid;
  d.q0_present = profile_valid;
  d.q0 = profile_valid ? profile.q0 : 0.0F;
  d.reference_kind = profile_valid ? profile.reference_kind
                                   : capture != nullptr ? capture->reference_kind
                                                        : Mq135ReferenceKind::None;
  double ratio{0.0};
  const bool ratio_ok = q_ok && adc_binding_ok &&
                        mq135_normalized_ratio_double(q, static_cast<double>(profile.q0), ratio);
  d.ratio_present = ratio_ok;
  d.ratio = ratio_ok ? ratio : 0.0;

  // ---- nh3 -------------------------------------------------------------
  d.curve_present = true;
  d.curve_id = kMq135Nh3CurveId;
  d.curve_version = kMq135Nh3CurveVersion;
  d.curve_source_sha256 = kMq135Nh3CurveSourceSha256;

  // ---- profiles --------------------------------------------------------
  const char* profile_state =
      !have_profile ? "MISSING" : !normalized_active ? "INVALID"
      : !profile_valid ? "INVALID"
      : profile.manifest.validity == ProfileValidity::TestOnly ? "TEST_ONLY" : "VALID";
  std::copy_n(profile_state,
              std::min(std::char_traits<char>::length(profile_state),
                       d.profile_state.size() - 1U),
              d.profile_state.begin());
  if (profile_valid) {
    d.profile = gas_active.ref;
  }
  if (profiles_ != nullptr) {
    ActiveProfileSnapshot previous{};
    if (profiles_->previous(kMq135GasKey, previous) && previous.present) {
      d.previous_profile = previous.ref;
    }
  }
  if (adc_profile_present) d.adc_profile = adc_active.ref;

  // ---- reference -------------------------------------------------------
  if (profile_valid) {
    d.reference_environment = profile.reference_environment;
    d.temperature_present = profile.temperature_present;
    d.temperature_centi_c = profile.temperature_centi_c;
    d.humidity_present = profile.humidity_present;
    d.humidity_centi_pct = profile.humidity_centi_pct;
    d.heating_duration_us = profile.heating_duration_us;
    d.preparation_confirmed = profile.preparation_confirmed;
    d.preparation_method = profile.preparation_method;
    d.sample_count = profile.sample_count;
    d.variation_percent = profile.variation_percent;
    d.calibration_recorded_at = profile.manifest.provenance.recorded_at;
    d.calibration_time_quality = profile.manifest.provenance.time_quality;
  } else if (capture != nullptr) {
    d.reference_environment = capture->reference_environment;
    d.temperature_present = capture->temperature_present;
    d.temperature_centi_c = capture->temperature_centi_c;
    d.humidity_present = capture->humidity_present;
    d.humidity_centi_pct = capture->humidity_centi_pct;
    d.heating_duration_us = capture->heating_duration_us;
    d.preparation_confirmed = capture->preparation_confirmed;
    d.preparation_method = capture->preparation_method;
    const auto summary = calibration_.normalized_summary();
    d.sample_count = summary.count;
    d.variation_percent = summary.variation_percent;
    if (source != nullptr) {
      d.calibration_recorded_at = source->source_recorded_at;
      d.calibration_time_quality = source->source_time_quality;
    }
  }
  d.conditions_declared = d.temperature_present || d.humidity_present;

  // ---- status precedence (documented in the producer report) -----------
  // 1 PROFILE_INVALID, 2 UNCALIBRATED, 3 ADC_CALIBRATION_UNAVAILABLE,
  // 4 SOURCE_NOT_READY, 5 SOURCE_STALE, 6 INVALID_RATIO,
  // 7 CURVE_UNAVAILABLE/INVALID, 8 REFERENCE_NOT_CLEAN_AIR/PREHEAT_UNCONFIRMED,
  // 9 BELOW_RANGE/ABOVE_RANGE/IN_RANGE.
  const char* quality = "UNCALIBRATED";
  const char* reason = "NO_NORMALIZED_PROFILE";
  const char* status = "UNCALIBRATED";
  if (normalized_active && !profile_valid) {
    quality = "INVALID"; reason = "NORMALIZED_PROFILE_INVALID"; status = "PROFILE_INVALID";
  } else if (!normalized_active && capture == nullptr) {
    quality = "UNCALIBRATED"; reason = "NO_NORMALIZED_PROFILE"; status = "UNCALIBRATED";
  } else if (!calibrated_ok) {
    quality = "UNAVAILABLE"; reason = "ADC_CALIBRATION_UNAVAILABLE";
    status = "ADC_CALIBRATION_UNAVAILABLE";
  } else if (source == nullptr) {
    quality = "DISCONNECTED"; reason = "NO_SOURCE"; status = "SOURCE_NOT_READY";
  } else if (!raw_present) {
    quality = "INVALID"; reason = "SOURCE_NOT_READY"; status = "SOURCE_NOT_READY";
  } else if (!source->identity_valid) {
    quality = "INVALID"; reason = "SOURCE_IDENTITY_INVALID"; status = "SOURCE_NOT_READY";
  } else if (source->quality != SampleQuality::Ready) {
    quality = source_quality(source->quality); reason = "SOURCE_NOT_READY";
    status = "SOURCE_NOT_READY";
  } else if (!clock_known) {
    quality = "INVALID"; reason = "SOURCE_CLOCK_UNKNOWN"; status = "SOURCE_NOT_READY";
  } else if (!fresh) {
    quality = "STALE"; reason = "SOURCE_STALE"; status = "SOURCE_STALE";
  } else if (!same_source_profile || !adc_binding_ok) {
    quality = "INVALID"; reason = "ADC_PROFILE_CHANGED"; status = "PROFILE_INVALID";
  } else if (!q_ok) {
    quality = "OUT_OF_RANGE"; reason = "AO_OUT_OF_RANGE"; status = "INVALID_RATIO";
  } else if (!ratio_ok) {
    quality = "INVALID"; reason = "BASELINE_MISSING"; status = "UNCALIBRATED";
  } else {
    const auto estimate = mq135_nh3_curve_estimate(kMq135Nh3Curve, ratio);
    switch (estimate.status) {
      case Mq135Nh3CurveStatus::None:
        d.ppm_present = true;
        d.ppm = estimate.ppm;
        quality = "READY"; reason = "IN_RANGE"; status = "IN_RANGE";
        break;
      case Mq135Nh3CurveStatus::RatioAboveDomain:
        quality = "READY"; reason = "BELOW_RANGE"; status = "BELOW_RANGE";
        break;
      case Mq135Nh3CurveStatus::RatioBelowDomain:
        quality = "READY"; reason = "ABOVE_RANGE"; status = "ABOVE_RANGE";
        break;
      case Mq135Nh3CurveStatus::InvalidRatio:
        quality = "INVALID"; reason = "INVALID_RATIO"; status = "INVALID_RATIO";
        break;
      case Mq135Nh3CurveStatus::CurveInvalid:
        quality = "INVALID"; reason = "CURVE_INVALID"; status = "CURVE_INVALID";
        break;
      case Mq135Nh3CurveStatus::CurveUnavailable:
        quality = "UNAVAILABLE"; reason = "CURVE_UNAVAILABLE"; status = "CURVE_UNAVAILABLE";
        break;
    }
    if (estimate.status == Mq135Nh3CurveStatus::None ||
        estimate.status == Mq135Nh3CurveStatus::RatioAboveDomain ||
        estimate.status == Mq135Nh3CurveStatus::RatioBelowDomain) {
      // A research estimate is only meaningful against a confirmed clean-air
      // baseline with a documented >48 h operator-declared initial preheat.
      if (d.reference_kind != Mq135ReferenceKind::CleanAirConfirmed) {
        d.ppm_present = false; d.ppm = 0.0;
        quality = "UNCALIBRATED"; reason = "REFERENCE_NOT_CLEAN_AIR";
        status = "REFERENCE_NOT_CLEAN_AIR";
      } else if (!d.preparation_confirmed ||
                 d.preparation_method != Mq135PreparationMethod::OperatorDeclared ||
                 d.heating_duration_us <= kMq135MinimumInitialPreheatUs) {
        d.ppm_present = false; d.ppm = 0.0;
        quality = "UNCALIBRATED"; reason = "PREHEAT_UNCONFIRMED";
        status = "PREHEAT_UNCONFIRMED";
      }
    }
  }
  std::copy_n(quality, std::min(std::char_traits<char>::length(quality),
                                d.quality.size() - 1U), d.quality.begin());
  std::copy_n(reason, std::min(std::char_traits<char>::length(reason),
                               d.reason.size() - 1U), d.reason.begin());
  std::copy_n(status, std::min(std::char_traits<char>::length(status),
                               d.nh3_status.size() - 1U), d.nh3_status.begin());
  return true;
}

bool CommandRouter::append_mq135_normalized_result(
    MaintenanceResponse& out, uint64_t now_us) const noexcept {
  acquisition::Mq135NormalizedDiagnostic snapshot{};
  return normalized_gas_diagnostic(calibration::CalibrationTarget::Mq135Gas, now_us,
                                   snapshot) &&
         append_mq135_normalized_result(out, snapshot);
}

bool CommandRouter::append_mq135_normalized_result(
    MaintenanceResponse& out,
    const acquisition::Mq135NormalizedDiagnostic& d) const noexcept {
  if (!d.present) return false;
  const bool profile_present = d.profile.present;
  // Root keys are comma-separated; group members use a comma-free separator so
  // the emitted document stays canonical with no duplicate/leading separators.
  return out.append("{\"schema\":\"MQG2\",\"sensor\":\"mq135\"") &&
    root_text(out, "quality", d.quality.data()) &&
    root_text(out, "reason", d.reason.data()) &&

    out.append(",\"adc\":{") &&
    member_small(out, "raw_counts", d.raw_counts, d.raw_present) &&
    member_bool(out, "available", d.adc_available) &&
    member_number(out, "pin_mv", d.pin_mv, d.pin_present) &&
    member_number(out, "ao_mv", d.ao_mv, d.ao_present) &&
    member_text(out, "method", d.adc_available ? "EFUSE_TP_FIT" : "UNAVAILABLE") &&
    member_bool(out, "same_sample", d.same_sample) &&
    member_text(out, "failure", d.adc_failure.data()) &&
    member_small(out, "resolution_bits", d.resolution_bits, d.adc_available) &&
    member_small(out, "attenuation_db", d.attenuation_db, d.adc_available) &&
    member_small(out, "adc_unit", d.adc_unit, d.adc_available) &&
    member_small(out, "adc_channel", d.adc_channel, d.adc_available) && out.append("}") &&

    out.append(",\"electrical\":{") &&
    member_small(out, "circuit_mv", d.circuit_mv, d.circuit_present) &&
    member_small(out, "divider_top_ohm", d.divider_top_ohm, d.circuit_present) &&
    member_small(out, "divider_bottom_ohm", d.divider_bottom_ohm, d.circuit_present) &&
    member_small(out, "topology", static_cast<uint32_t>(d.topology), d.circuit_present) &&
    member_bool(out, "hardware_confirmed", d.hardware_confirmed) &&
    member_text(out, "hardware_identity", d.hardware_identity.data(), d.circuit_present) &&
    out.append("}") &&

    out.append(",\"response\":{") &&
    member_number(out, "q", d.q, d.q_present) &&
    member_number(out, "q0", d.q0, d.q0_present) &&
    member_number(out, "ratio", d.ratio, d.ratio_present) &&
    member_text(out, "reference_kind", reference_kind(d.reference_kind)) &&
    member_bool(out, "baseline_valid", d.baseline_valid) && out.append("}") &&

    out.append(",\"nh3\":{") &&
    member_number(out, "ppm", d.ppm, d.ppm_present) &&
    member_text(out, "status", d.nh3_status.data()) &&
    member_small(out, "curve_id", d.curve_id, d.curve_present) &&
    member_small(out, "curve_version", d.curve_version, d.curve_present) &&
    member_digest(out, "source_sha256", d.curve_source_sha256.data(),
                  d.curve_source_sha256.size(), d.curve_present, true) &&
    member_text(out, "method", "PIECEWISE_LOG_LOG") &&
    member_bool(out, "production_eligible", false) && out.append("}") &&

    out.append(",\"profiles\":{") &&
    member_text(out, "state", d.profile_state.data()) &&
    member_u64(out, "version", d.profile.profile_version, profile_present) &&
    member_digest(out, "hash", d.profile.hash.data(), 32U, profile_present, true) &&
    member_u64(out, "previous_version", d.previous_profile.profile_version,
               d.previous_profile.present) &&
    member_digest(out, "previous_hash", d.previous_profile.hash.data(), 32U,
                  d.previous_profile.present, true) &&
    member_u64(out, "adc_version", d.adc_profile.profile_version,
               d.adc_profile.present) &&
    member_digest(out, "adc_hash", d.adc_profile.hash.data(), 32U,
                  d.adc_profile.present, true) && out.append("}") &&

    out.append(",\"source\":{") &&
    member_digest(out, "boot_id", d.source_boot_id.bytes.data(), 16U,
                  d.source_present && d.source_boot_id.present, false) &&
    member_u64(out, "sequence", d.source_sequence, d.source_present) &&
    member_u64(out, "monotonic_us", d.source_us, d.source_present) &&
    member_u64(out, "response_monotonic_us", d.response_us, true) &&
    member_u64(out, "age_us", d.age_known ? d.response_us - d.source_us : 0U,
               d.age_known) &&
    member_u64(out, "max_age_us", d.max_age_us, d.age_known) &&
    member_nullable_bool(out, "fresh", d.fresh, d.age_known) &&
    member_text(out, "quality", d.source_quality.data(), d.source_present) &&
    out.append("}") &&

    out.append(",\"reference\":{") &&
    member_text(out, "environment", d.reference_environment.data(),
                d.circuit_present) &&
    member_number(out, "temperature_centi_c", d.temperature_centi_c,
                  d.temperature_present) &&
    member_number(out, "humidity_centi_pct", d.humidity_centi_pct,
                  d.humidity_present) &&
    member_text(out, "conditions_source",
                d.conditions_declared ? "DECLARED" : "UNKNOWN") &&
    member_u64(out, "heating_duration_us", d.heating_duration_us,
               d.circuit_present) &&
    member_bool(out, "preparation_confirmed", d.preparation_confirmed) &&
    member_text(out, "preparation_method", preparation_method(d.preparation_method)) &&
    member_small(out, "sample_count", d.sample_count, profile_present) &&
    member_number(out, "variation_percent", d.variation_percent, profile_present) &&
    member_i64(out, "calibration_epoch_ms", d.calibration_recorded_at.epoch_ms,
               profile_present && d.calibration_recorded_at.available) &&
    member_text(out, "calibration_time_quality",
                time_quality(d.calibration_time_quality)) &&
    // Close the `reference` group and then the root object. Without the second
    // brace the emitted MQG2 document is not valid JSON at all.
    out.append("}}");
}

}  // namespace sigap::maintenance
