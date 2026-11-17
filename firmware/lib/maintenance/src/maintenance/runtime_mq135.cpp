#include "maintenance/runtime.h"
#include <charconv>
#include <cmath>
#include <cstdio>

namespace sigap::maintenance {
namespace {
bool label(MaintenanceResponse& out, const char* key) noexcept {
  return out.append(",\"") && out.append(key) && out.append("\":");
}
bool text(MaintenanceResponse& out, const char* key, std::string_view value, bool present = true) noexcept {
  if (!label(out, key)) return false;
  if (!present) return out.append("null");
  if (!out.append("\"")) return false;
  for (const unsigned char c : value) {
    if (c < 32) return false;
    if ((c == '\\' || c == '"') && !out.append("\\")) return false;
    const char ch = static_cast<char>(c);
    if (!out.append(std::string_view(&ch, 1))) return false;
  }
  return out.append("\"");
}
bool number(MaintenanceResponse& out, const char* key, double value, bool present) noexcept {
  if (!label(out, key)) return false;
  if (!present) return out.append("null");
  if (!std::isfinite(value)) return false;
  char buffer[32]{}; const auto n = std::snprintf(buffer, sizeof(buffer), "%.9g", value);
  return n > 0 && static_cast<std::size_t>(n) < sizeof(buffer) && out.append(std::string_view(buffer, n));
}
bool integer(MaintenanceResponse& out, const char* key, uint64_t value, bool present) noexcept {
  char buffer[24]{}; const auto n = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return n.ec == std::errc{} && text(out, key, std::string_view(buffer, n.ptr - buffer), present);
}
bool signed_integer(MaintenanceResponse& out, const char* key, int64_t value, bool present) noexcept {
  char buffer[24]{}; const auto n = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return n.ec == std::errc{} && text(out, key, std::string_view(buffer, n.ptr - buffer), present);
}
bool digest(MaintenanceResponse& out, const char* key, const uint8_t* bytes,
            std::size_t size, bool present, bool hash) noexcept {
  if (!label(out, key)) return false;
  if (!present) return out.append("null");
  if (!out.append(hash ? "\"sha256:" : "\"")) return false;
  constexpr char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < size; ++i) {
    const char pair[]{digits[bytes[i] >> 4], digits[bytes[i] & 15]};
    if (!out.append(std::string_view(pair, 2))) return false;
  }
  return out.append("\"");
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
}

bool CommandRouter::gas_diagnostic(calibration::CalibrationTarget target, uint64_t now_us,
                                    acquisition::Mq135Diagnostic& d) const noexcept {
  if (target != calibration::CalibrationTarget::Mq135Gas) return false;
  d = acquisition::Mq135Diagnostic{};
  using namespace acquisition;
  calibration::TrialObservation observation{}; uint64_t sequence{};
  const bool have_source = evidence_ != nullptr && evidence_->latest(calibration::CalibrationTarget::Mq135Gas, 0, observation, sequence);
  const auto* source = have_source ? std::get_if<calibration::MqTrial>(&observation) : nullptr;
  ActiveProfileSnapshot gas_active{}, adc_active{}; ProfileCodec codec{};
  Mq135GasProfile gas{}; MqAdcProfile adc{};
  const bool have_profile = profiles_ != nullptr && profiles_->snapshot(kMq135GasKey, gas_active) && gas_active.present;
  const bool profile_valid = have_profile && codec.decode(gas_active.encoded, gas) && gas.valid();
  const bool adc_valid = profiles_ != nullptr && profiles_->snapshot(ProfileTarget::Mq135, adc_active) &&
    adc_active.present && codec.decode(adc_active.encoded, adc) && adc.valid_for_conversion() &&
    adc.manifest.provenance.present && adc.manifest.provenance.evidence_count > 0;
  const auto* capture = calibration_.target() == calibration::CalibrationTarget::Mq135Gas &&
    calibration_.state() != calibration::CalibrationState::Idle && calibration_.state() != calibration::CalibrationState::Failed
      ? calibration_.gas_criteria() : nullptr;
  if (!profile_valid && capture != nullptr) gas = capture->profile;
  const bool circuit_present = profile_valid || capture != nullptr;
  const bool clock_known = source != nullptr && boot_id_ != nullptr && boot_id_->present &&
    source->source_boot_id.present && source->source_boot_id.bytes == boot_id_->bytes && now_us >= source->observed_at_us;
  const uint64_t age = clock_known ? now_us - source->observed_at_us : 0;
  const bool age_known = clock_known && circuit_present && gas.criteria.max_age_us > 0;
  const bool fresh = age_known && age <= gas.criteria.max_age_us;
  const bool raw_present = source != nullptr && source->raw_present;
  const bool same_adc = source != nullptr && adc_valid && source->source_profile.present &&
    source->source_profile.kind == ProfileKind::Adc && source->source_profile.target_id == 8 &&
    source->source_profile.hash == adc.manifest.hash && source->source_profile.profile_version == adc.manifest.profile_version;
  const bool binding_valid = !profile_valid || (gas.adc_hash == adc.manifest.hash && gas.adc_version == adc.manifest.profile_version);
  const bool voltage_present = raw_present && same_adc && source->identity_valid &&
    source->adc_counts <= adc.full_scale_counts && adc.divider_numerator > 0 && adc.divider_denominator > 0;
  const double pin = voltage_present ? static_cast<double>(source->adc_counts) * adc.reference_mv / adc.full_scale_counts : 0;
  const double ao = voltage_present ? pin * adc.divider_denominator / adc.divider_numerator : 0;
  Mq135Resistance resistance{};
  if (raw_present && same_adc && binding_valid && circuit_present && fresh && source->identity_valid &&
      source->quality == SampleQuality::Ready) resistance = mq135_resistance(source->adc_counts, adc, gas.electrical);
  float ratio{};
  const bool ratio_present = resistance.ready() && profile_valid && mq135_ratio(resistance.rs_ohm, gas.r0_ohm, ratio);
  const char* quality = "UNCALIBRATED"; const char* reason = "GAS_PROFILE_MISSING";
  if (source == nullptr) { quality = "DISCONNECTED"; reason = "NO_SOURCE"; }
  else if (!raw_present) { quality = "INVALID"; reason = "RAW_MISSING"; }
  else if (!source->identity_valid) { quality = "INVALID"; reason = "SOURCE_IDENTITY_INVALID"; }
  else if (!adc_valid) { reason = "ADC_UNVERIFIED"; }
  else if (!same_adc || !binding_valid) { quality = "INVALID"; reason = "ADC_PROFILE_CHANGED"; }
  else if (source->quality != SampleQuality::Ready) { quality = source_quality(source->quality); reason = "SOURCE_NOT_READY"; }
  else if (!clock_known) { quality = "INVALID"; reason = "SOURCE_CLOCK_UNKNOWN"; }
  else if (age_known && !fresh) { quality = "STALE"; reason = "SOURCE_STALE"; }
  else if (have_profile && !profile_valid) { quality = "INVALID"; reason = "GAS_PROFILE_INVALID"; }
  else if (ratio_present) { quality = "READY"; reason = "NONE"; }
  else if (capture != nullptr && resistance.ready()) { quality = "CALIBRATING"; reason = "REFERENCE_PENDING"; }
  else if (circuit_present && !resistance.ready()) {
    quality = "INVALID";
    switch (resistance.reason) {
      case Mq135GasReason::Saturated: quality = "OUT_OF_RANGE"; reason = "ADC_SATURATED"; break;
      case Mq135GasReason::DividerMismatch: reason = "DIVIDER_MISMATCH"; break;
      case Mq135GasReason::VoltageInvalid: quality = "OUT_OF_RANGE"; reason = "AO_OUT_OF_RANGE"; break;
      case Mq135GasReason::ElectricalUnverified: reason = "ELECTRICAL_UNVERIFIED"; break;
      default: reason = "ELECTRICAL_OR_SOURCE_INVALID"; break;
    }
  }
  const auto assign = [](auto& output, const char* value) noexcept {
    std::copy_n(value, std::min(std::char_traits<char>::length(value), output.size() - 1), output.begin());
  };
  d.present = true; d.source_present = source != nullptr; d.raw_present = raw_present;
  d.voltage_present = voltage_present; d.rs_present = resistance.ready(); d.ratio_present = ratio_present;
  d.profile_present = profile_valid; d.circuit_present = circuit_present;
  d.clock_known = clock_known; d.age_known = age_known; d.fresh = fresh;
  assign(d.quality, quality); assign(d.reason, reason);
  assign(d.profile_state, !have_profile ? "MISSING" : !profile_valid ? "INVALID" :
    gas.manifest.validity == ProfileValidity::TestOnly ? "TEST_ONLY" : "VALID");
  d.raw_counts = raw_present ? source->adc_counts : 0;
  d.adc_pin_mv = static_cast<float>(pin); d.ao_mv = static_cast<float>(ao);
  d.rs_ohm = resistance.rs_ohm; d.r0_ohm = gas.r0_ohm; d.ratio = ratio;
  d.electrical = gas.electrical; d.hardware_identity = gas.hardware_identity;
  d.reference_environment = gas.reference_environment;
  d.temperature_centi_c = gas.temperature_centi_c; d.humidity_centi_pct = gas.humidity_centi_percent;
  d.sample_count = gas.reference.count; d.variation_percent = gas.reference.variation_percent;
  d.profile = profile_valid ? gas_active.ref : ProfileRef{};
  ActiveProfileSnapshot previous{};
  if (profiles_ != nullptr && profiles_->previous(kMq135GasKey, previous) && previous.present) d.previous_profile = previous.ref;
  d.adc_profile = adc_valid ? adc_active.ref : ProfileRef{};
  if (source != nullptr) {
    d.source_boot_id = source->source_boot_id; d.source_sequence = source->source_sequence;
    d.source_us = source->observed_at_us;
  }
  d.response_us = now_us; d.age_us = age; d.max_age_us = gas.criteria.max_age_us;
  d.calibration_recorded_at = gas.manifest.provenance.recorded_at;
  d.calibration_time_quality = gas.manifest.provenance.time_quality;
  return true;
}

bool CommandRouter::append_mq135_gas_result(MaintenanceResponse& out, uint64_t now_us) const noexcept {
  acquisition::Mq135Diagnostic snapshot{};
  return gas_diagnostic(calibration::CalibrationTarget::Mq135Gas, now_us, snapshot) && append_mq135_gas_result(out, snapshot);
}

bool CommandRouter::append_mq135_gas_result(MaintenanceResponse& out,
                                           const acquisition::Mq135Diagnostic& d) const noexcept {
  if (!d.present) return false;
  return out.append("{\"schema\":\"MQG1\",\"sensor\":\"mq135\"") &&
    text(out, "quality", d.quality.data()) && text(out, "reason", d.reason.data()) &&
    number(out, "raw_counts", d.raw_counts, d.raw_present) && text(out, "adc_method", "PROFILE_LINEAR") &&
    number(out, "adc_pin_mv", d.adc_pin_mv, d.voltage_present) && number(out, "ao_mv", d.ao_mv, d.voltage_present) &&
    number(out, "rs_ohm", d.rs_ohm, d.rs_present) && number(out, "r0_ohm", d.r0_ohm, d.profile_present) &&
    number(out, "rs_over_r0", d.ratio, d.ratio_present) && out.append(",\"nh3_ppm\":null,\"nh3_status\":\"UNCALIBRATED\"") &&
    text(out, "profile_state", d.profile_state.data()) &&
    integer(out, "profile_version", d.profile.profile_version, d.profile.present) &&
    digest(out, "profile_hash", d.profile.hash.data(), 32, d.profile.present, true) &&
    integer(out, "previous_profile_version", d.previous_profile.profile_version, d.previous_profile.present) &&
    digest(out, "previous_profile_hash", d.previous_profile.hash.data(), 32, d.previous_profile.present, true) &&
    integer(out, "adc_profile_version", d.adc_profile.profile_version, d.adc_profile.present) &&
    digest(out, "adc_profile_hash", d.adc_profile.hash.data(), 32, d.adc_profile.present, true) &&
    integer(out, "source_sequence", d.source_sequence, d.source_present) &&
    digest(out, "source_boot_id", d.source_boot_id.bytes.data(), 16, d.source_boot_id.present, false) &&
    integer(out, "source_monotonic_us", d.source_us, d.source_present) &&
    integer(out, "response_monotonic_us", d.response_us, true) &&
    integer(out, "age_us", d.age_us, d.clock_known) && integer(out, "max_age_us", d.max_age_us, d.age_known) &&
    label(out, "fresh") && out.append(!d.age_known ? "null" : d.fresh ? "true" : "false") &&
    text(out, "hardware_identity", d.hardware_identity.data(), d.circuit_present) &&
    text(out, "reference_environment", d.reference_environment.data(), d.circuit_present) &&
    number(out, "reference_temperature_centi_c", d.temperature_centi_c, d.circuit_present) &&
    number(out, "reference_humidity_centi_pct", d.humidity_centi_pct, d.circuit_present) &&
    signed_integer(out, "calibration_epoch_ms", d.calibration_recorded_at.epoch_ms, d.profile_present && d.calibration_recorded_at.available) &&
    text(out, "calibration_time_quality", d.calibration_time_quality == acquisition::TimeQuality::Synchronized ? "SYNCHRONIZED" :
      d.calibration_time_quality == acquisition::TimeQuality::Estimated ? "ESTIMATED" : "UNSYNCED") &&
    number(out, "sample_count", d.sample_count, d.profile_present) &&
    number(out, "variation_percent", d.variation_percent, d.profile_present) &&
    number(out, "circuit_mv", d.electrical.circuit_mv, d.circuit_present) &&
    number(out, "module_load_ohm", d.electrical.module_load_ohm, d.circuit_present) &&
    number(out, "divider_top_ohm", d.electrical.divider_top_ohm, d.circuit_present) &&
    number(out, "divider_bottom_ohm", d.electrical.divider_bottom_ohm, d.circuit_present) &&
    number(out, "topology", static_cast<unsigned>(d.electrical.topology), d.circuit_present) && out.append("}");
}
} // namespace sigap::maintenance
