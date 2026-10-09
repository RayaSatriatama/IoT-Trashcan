#include "maintenance/runtime.h"

#include "acquisition/target_protocol.h"

#include <cmath>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "telemetry/sha256.h"

namespace sigap::maintenance {
namespace {

const char* calibration_target_name(
    calibration::CalibrationTarget target) noexcept {
  constexpr const char* names[] = {
      "hx711_organic", "hx711_inorganic", "hx711_b3",
      "ultrasonic_organic", "ultrasonic_inorganic", "ultrasonic_b3",
      "mq4", "mq135", "mq7", "mq8", "ds18b20_b3", "sht31_44",
      "sht31_45"};
  const auto index = static_cast<std::size_t>(target);
  return index < (sizeof(names) / sizeof(names[0])) ? names[index] : "";
}

MaintenanceCommand typed_command(CommandKind kind,
                                 calibration::CalibrationTarget target) noexcept {
  MaintenanceCommand command{};
  command.kind = kind;
  (void)command.target.assign(calibration_target_name(target));
  return command;
}

bool typed_dispatch(CommandRouter& router, MaintenanceCommand command,
                    uint64_t now_us) noexcept {
  MaintenanceResponse response{};
  return router.execute(command, now_us, response) == CommandStatus::Accepted;
}

void set_calibration_provenance(acquisition::ProfileProvenance& provenance,
                                const acquisition::BootId* boot_id,
                                uint64_t now_us,
                                int64_t primary,
                                int64_t secondary,
                                uint32_t evidence_count,
                                uint64_t evidence_sequence,
                                uint8_t units) noexcept {
  provenance = acquisition::ProfileProvenance{};
  provenance.present = true;
  if (boot_id != nullptr) provenance.boot_id = *boot_id;
  provenance.monotonic_us = now_us;
  provenance.time_quality = acquisition::TimeQuality::Unsynced;
  provenance.reference_primary = primary;
  provenance.reference_secondary = secondary;
  provenance.evidence_count = evidence_count;
  provenance.evidence_sequence = evidence_sequence;
  provenance.units = units;
  constexpr char note[] = "calibration-evidence";
  std::memcpy(provenance.reference_note.data(), note, sizeof(note) - 1U);
}

bool append_i64(MaintenanceResponse& response, const char* label,
                int64_t value) noexcept {
  char number[24]{};
  const auto result = std::to_chars(number, number + sizeof(number), value);
  return result.ec == std::errc{} && response.append(label) &&
         response.append(std::string_view(number, result.ptr - number)) &&
         response.append("\n");
}

bool append_u32(MaintenanceResponse& response, const char* label,
                uint32_t value) noexcept {
  char number[16]{};
  const auto result = std::to_chars(number, number + sizeof(number), value);
  return result.ec == std::errc{} && response.append(label) &&
         response.append(std::string_view(number, result.ptr - number)) &&
         response.append("\n");
}

bool append_u64(MaintenanceResponse& response, const char* label,
                uint64_t value) noexcept {
  char number[24]{};
  const auto result = std::to_chars(number, number + sizeof(number), value);
  return result.ec == std::errc{} && response.append(label) &&
         response.append(std::string_view(number, result.ptr - number)) &&
         response.append("\n");
}

bool append_hex(MaintenanceResponse& response, const uint8_t* bytes,
                std::size_t size) noexcept {
  static constexpr char digits[] = "0123456789abcdef";
  for (std::size_t index = 0U; index < size; ++index) {
    char pair[2]{digits[bytes[index] >> 4U], digits[bytes[index] & 0x0fU]};
    if (!response.append(std::string_view(pair, 2U))) return false;
  }
  return true;
}

bool append_float(MaintenanceResponse& response, const char* label,
                  float value) noexcept {
  char number[32]{};
  const int written = std::snprintf(number, sizeof(number), "%.3f",
                                    static_cast<double>(value));
  return written > 0 && static_cast<std::size_t>(written) < sizeof(number) &&
         response.append(label) &&
         response.append(std::string_view(number, static_cast<std::size_t>(written))) &&
         response.append("\n");
}

bool append_rom(MaintenanceResponse& response, const std::array<uint8_t, 8U>& rom) noexcept {
  static constexpr char kDigits[] = "0123456789abcdef";
  char text[16]{};
  for (std::size_t index = 0U; index < rom.size(); ++index) {
    text[index * 2U] = kDigits[rom[index] >> 4U];
    text[index * 2U + 1U] = kDigits[rom[index] & 0x0fU];
  }
  return response.append("rom=") && response.append(std::string_view(text, sizeof(text))) &&
         response.append("\n");
}

bool append_preview_metadata(MaintenanceResponse& response,
                             uint32_t evidence_count,
                             uint64_t evidence_sequence) noexcept;

bool append_proposal_identity(MaintenanceResponse& response,
                              const acquisition::EncodedProfile& encoded,
                              uint32_t version, uint32_t evidence_count,
                              uint64_t evidence_sequence) noexcept {
  return response.append("proposal_hash=sha256:") &&
         append_hex(response, encoded.hash().data(), encoded.hash().size()) &&
         response.append("\n") && append_u32(response, "profile_version=", version) &&
         append_preview_metadata(response, evidence_count, evidence_sequence);
}

bool append_preview_metadata(MaintenanceResponse& response,
                             uint32_t evidence_count,
                             uint64_t evidence_sequence) noexcept {
  char sequence[24]{};
  const auto converted = std::to_chars(sequence, sequence + sizeof(sequence),
                                       evidence_sequence);
  if (converted.ec != std::errc{}) return false;
  const std::string_view sequence_view(sequence, converted.ptr - sequence);
  char summary[64]{};
  const int written = std::snprintf(summary, sizeof(summary),
                                    "evidence:%.*s:%u",
                                    static_cast<int>(sequence_view.size()),
                                    sequence_view.data(), evidence_count);
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(summary)) return false;
  const auto digest = telemetry_contract::sha256(
      std::string_view(summary, static_cast<std::size_t>(written)));
  return append_u32(response, "evidence_count=", evidence_count) &&
         append_u64(response, "evidence_sequence=", evidence_sequence) &&
         response.append("evidence_hash=sha256:") &&
         append_hex(response, digest.data(), digest.size()) &&
         response.append("\n") &&
         append_u32(response, "expires_in_ms=",
                    static_cast<uint32_t>(calibration::CalibrationSession::kSessionTimeoutUs / 1000U));
}

bool same_active_profile(const acquisition::ActiveProfileSnapshot& left,
                         const acquisition::ActiveProfileSnapshot& right) noexcept {
  if (left.present != right.present) return false;
  if (!left.present) return true;
  return left.ref.kind == right.ref.kind &&
         left.ref.schema_version == right.ref.schema_version &&
         left.ref.profile_version == right.ref.profile_version &&
         left.ref.target_id == right.ref.target_id &&
         left.ref.hash == right.ref.hash &&
         left.encoded.size == right.encoded.size &&
         std::memcmp(left.encoded.bytes.data(), right.encoded.bytes.data(),
                     left.encoded.size) == 0;
}

}  // namespace

bool CommandRouter::status(
    calibration::CalibrationStatusSnapshot& output) const noexcept {
  output = calibration::CalibrationStatusSnapshot{};
  output.target = calibration_.target();
  output.state = calibration_.state();
  output.failure = calibration_.failure();
  output.active = output.state != calibration::CalibrationState::Idle &&
                  output.state != calibration::CalibrationState::Failed;
  output.last_activity_us = calibration_.last_activity_us();
  output.accepted_samples = calibration_.stats().count +
                            calibration_.secondary_stats().count;
  output.rejected_samples = calibration_.stats().quality_failures +
                            calibration_.secondary_stats().quality_failures;
  return true;
}

bool CommandRouter::evidence(
    calibration::CalibrationTarget target,
    calibration::DiagnosticEvidence& output) const noexcept {
  return evidence_ != nullptr && evidence_->diagnostic(target, output);
}

bool CommandRouter::active_profile(
    calibration::CalibrationTarget target,
    calibration::CalibrationProfileRef& output) const noexcept {
  output = calibration::CalibrationProfileRef{};
  if (profiles_ == nullptr) return false;
  const auto profile_target = static_cast<acquisition::ProfileTarget>(
      static_cast<uint8_t>(target) + 1U);
  acquisition::ActiveProfileSnapshot active{};
  if (!profiles_->snapshot(acquisition::default_profile_key(profile_target),
                           active) || !active.present) {
    return true;
  }
  output.present = true;
  output.version = active.ref.profile_version;
  output.hash = active.ref.hash;
  return true;
}

bool CommandRouter::proposed_profile(
    calibration::CalibrationProfileRef& output) const noexcept {
  output = calibration::CalibrationProfileRef{};
  if (!calibration_proposal_.present) return false;
  output.hash = calibration_proposal_.encoded.hash();
  MaintenanceResponse preview{};
  if (!append_calibration_preview(preview)) return false;
  const auto text = preview.view();
  const auto marker = text.find("profile_version=");
  if (marker == std::string_view::npos) return false;
  const auto begin = marker + std::string_view("profile_version=").size();
  const auto end = text.find('\n', begin);
  const auto value = text.substr(begin, end == std::string_view::npos
                                          ? text.size() - begin
                                          : end - begin);
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                      output.version);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      output.version == 0U) {
    output = calibration::CalibrationProfileRef{};
    return false;
  }
  output.present = true;
  return true;
}

bool CommandRouter::start(calibration::CalibrationTarget target,
                          uint64_t now_us) noexcept {
  return typed_dispatch(*this,
                        typed_command(CommandKind::CalibrationStart, target),
                        now_us);
}

bool CommandRouter::set_criteria(const calibration::CaptureCriteria& criteria,
                                 uint64_t now_us) noexcept {
  auto set_numeric = [&](const char* field, int64_t value) noexcept {
    MaintenanceCommand command = typed_command(CommandKind::CalibrationCriteria,
                                               calibration_.target());
    if (!command.field.assign(field)) return false;
    command.numeric_value = value;
    command.has_numeric = true;
    return typed_dispatch(*this, command, now_us);
  };
  return std::visit(
      [&](const auto& value) noexcept {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, calibration::Hx711CaptureCriteria>) {
          return set_numeric("samples", value.required_samples) &&
                 set_numeric("max_span_counts", value.max_span_counts);
        } else if constexpr (std::is_same_v<T, calibration::DistanceCaptureCriteria>) {
          return set_numeric("samples", value.required_samples) &&
                 set_numeric("max_span", static_cast<int64_t>(value.max_span_mm));
        } else if constexpr (std::is_same_v<T, calibration::MqCaptureCriteria>) {
          return set_numeric("samples", value.required_samples) &&
                 set_numeric("max_span_counts", value.max_span_counts);
        } else if constexpr (std::is_same_v<T, calibration::Ds18b20CaptureCriteria>) {
          return set_numeric("samples", value.required_samples) &&
                 set_numeric("max_span", value.max_span_centi_c);
        } else {
          return set_numeric("samples", value.required_samples) &&
                 set_numeric("max_temperature_span_centi_c",
                             value.max_temperature_span_centi_c) &&
                 set_numeric("max_humidity_span_centi_pct",
                             value.max_humidity_span_centi_pct);
        }
      },
      criteria);
}

bool CommandRouter::capture(calibration::CalibrationAnchor anchor,
                            uint64_t now_us) noexcept {
  MaintenanceCommand command = typed_command(CommandKind::CalibrationCapture,
                                             calibration_.target());
  const char* name = anchor == calibration::CalibrationAnchor::Tare
                         ? "tare"
                         : anchor == calibration::CalibrationAnchor::Reference
                               ? "reference"
                               : anchor == calibration::CalibrationAnchor::Empty
                                     ? "empty"
                                     : anchor == calibration::CalibrationAnchor::Full
                                           ? "full"
                                           : "primary";
  if (!command.target.assign(name)) return false;
  return typed_dispatch(*this, command, now_us);
}

bool CommandRouter::validate() noexcept {
  MaintenanceCommand command = typed_command(CommandKind::CalibrationValidate,
                                             calibration_.target());
  return typed_dispatch(*this, command, calibration_.last_activity_us());
}

bool CommandRouter::preview(calibration::ValidationResult& output) noexcept {
  MaintenanceCommand command = typed_command(CommandKind::CalibrationPreview,
                                             calibration_.target());
  const bool accepted = typed_dispatch(*this, command,
                                       calibration_.last_activity_us());
  output = calibration_.validation();
  return accepted;
}

bool CommandRouter::commit() noexcept {
  MaintenanceCommand command = typed_command(CommandKind::CalibrationCommit,
                                             calibration_.target());
  return typed_dispatch(*this, command, calibration_.last_activity_us());
}

bool CommandRouter::cancel() noexcept {
  MaintenanceCommand command = typed_command(CommandKind::CalibrationCancel,
                                             calibration_.target());
  return typed_dispatch(*this, command, calibration_.last_activity_us());
}

bool CommandRouter::history(calibration::CalibrationResultEnvelope&) const noexcept {
  return false;
}

bool CommandRouter::rollback() noexcept { return false; }

bool CommandRouter::append_calibration_preview_response(
    MaintenanceResponse& response) const noexcept {
  return append_calibration_preview(response);
}

bool CommandRouter::build_calibration_proposal(
    uint64_t now_us, CalibrationProposal& proposal) const noexcept {
  if (profiles_ == nullptr) return false;
  const auto target = static_cast<acquisition::ProfileTarget>(
      static_cast<uint8_t>(pending_target_) + 1U);
  const acquisition::ProfileKey key{
      (pending_target_ >= calibration::CalibrationTarget::Mq4 &&
       pending_target_ <= calibration::CalibrationTarget::Mq8)
          ? acquisition::ProfileKind::Adc
          : acquisition::ProfileKind::Calibration,
      target};
  acquisition::ActiveProfileSnapshot active{};
  if (!profiles_->snapshot(key, active)) return false;
  if (active.present && active.ref.profile_version == UINT32_MAX) return false;
  const uint32_t version = active.present ? active.ref.profile_version + 1U : 1U;
  acquisition::ProfileCodec codec{};
  acquisition::EncodedProfile encoded{};
  switch (pending_target_) {
    case calibration::CalibrationTarget::Hx711Organic:
    case calibration::CalibrationTarget::Hx711Inorganic:
    case calibration::CalibrationTarget::Hx711B3: {
      if (reference_mass_mg_ == 0U || calibration_.secondary_stats().count == 0U) return false;
      acquisition::Hx711Profile profile{};
      profile.manifest.kind = acquisition::ProfileKind::Calibration;
      profile.manifest.profile_version = version;
      profile.manifest.target_id = static_cast<uint32_t>(target);
      profile.manifest.validity = acquisition::ProfileValidity::Valid;
      profile.tare_offset_counts = static_cast<int64_t>(calibration_.stats().mean);
      const double kg = static_cast<double>(reference_mass_mg_) / 1000000.0;
      const double delta = calibration_.secondary_stats().mean - calibration_.stats().mean;
      profile.counts_per_kg = static_cast<float>(delta / kg);
      set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                 static_cast<int64_t>(profile.tare_offset_counts),
                                 static_cast<int64_t>(reference_mass_mg_),
                                 calibration_.stats().count +
                                     calibration_.secondary_stats().count,
                                 evidence_sequence_, 1U);
      if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      break;
    }
    case calibration::CalibrationTarget::UltrasonicOrganic:
    case calibration::CalibrationTarget::UltrasonicInorganic:
    case calibration::CalibrationTarget::UltrasonicB3: {
      acquisition::UltrasonicProfile profile{};
      if (active.present) {
        if (!codec.decode(active.encoded, profile)) return false;
      } else {
        if (!ultrasonic_echo_conversion_set_ || !ultrasonic_timing_set_) {
          return false;
        }
        profile.echo_mm_per_us =
            static_cast<float>(ultrasonic_echo_mm_per_us_milli_) / 1000.0F;
        profile.echo_timeout_us = ultrasonic_echo_timeout_us_;
        profile.inter_sensor_gap_us = ultrasonic_inter_sensor_gap_us_;
        profile.overshoot_tolerance_mm =
            static_cast<float>(ultrasonic_overshoot_tolerance_mm_milli_) /
            1000.0F;
      }
      if (calibration_.secondary_stats().count == 0U) return false;
      profile.manifest.kind = acquisition::ProfileKind::Calibration;
      profile.manifest.schema_version = acquisition::kProfileSchemaVersion;
      profile.manifest.target_id = static_cast<uint32_t>(target);
      profile.manifest.profile_version = version;
      profile.d_empty_mm = static_cast<float>(calibration_.stats().mean);
      profile.d_full_mm = static_cast<float>(calibration_.secondary_stats().mean);
      profile.manifest.validity = acquisition::ProfileValidity::Valid;
      set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                 static_cast<int64_t>(calibration_.stats().mean),
                                 static_cast<int64_t>(calibration_.secondary_stats().mean),
                                 calibration_.stats().count +
                                     calibration_.secondary_stats().count,
                                 evidence_sequence_, 2U);
      if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      break;
    }
    default:
      if (pending_target_ == calibration::CalibrationTarget::Ds18b20B3) {
        if (!reference_temperature_set_) return false;
        acquisition::Ds18b20Profile profile{};
        if (active.present) {
          if (!codec.decode(active.encoded, profile)) return false;
        } else {
          if (!ds_rom_set_ || ds_rom_[0] != 0x28U ||
              acquisition::ds18b20_crc8(ds_rom_.data(), 7U) != ds_rom_[7]) {
            return false;
          }
          profile.has_rom = true;
          profile.rom = ds_rom_;
          profile.resolution_bits = ds_resolution_set_ ? ds_resolution_bits_ : 12U;
        }
        profile.manifest.kind = acquisition::ProfileKind::Calibration;
        profile.manifest.schema_version = acquisition::kProfileSchemaVersion;
        profile.manifest.target_id = static_cast<uint32_t>(target);
        profile.manifest.validity = acquisition::ProfileValidity::Valid;
        profile.manifest.profile_version = version;
        profile.offset_centi_c = reference_temperature_centi_c_ -
                                 static_cast<int32_t>(calibration_.stats().mean);
        set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                   reference_temperature_centi_c_, 0,
                                   calibration_.stats().count, evidence_sequence_, 3U);
        if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      } else if (pending_target_ == calibration::CalibrationTarget::Sht31_44 ||
                 pending_target_ == calibration::CalibrationTarget::Sht31_45) {
        if (!reference_temperature_set_ || !reference_humidity_set_) return false;
        acquisition::Sht31Profile profile{};
        if (active.present) {
          if (!codec.decode(active.encoded, profile)) return false;
        } else {
          profile.address = pending_target_ == calibration::CalibrationTarget::Sht31_45
                                ? 0x45U
                                : 0x44U;
        }
        profile.manifest.kind = acquisition::ProfileKind::Calibration;
        profile.manifest.schema_version = acquisition::kProfileSchemaVersion;
        profile.manifest.target_id = static_cast<uint32_t>(target);
        profile.manifest.validity = acquisition::ProfileValidity::Valid;
        profile.manifest.profile_version = version;
        profile.temperature_offset_centi_c =
            reference_temperature_centi_c_ -
            static_cast<int32_t>(calibration_.stats().mean);
        profile.humidity_offset_centi_pct =
            reference_humidity_centi_pct_ -
            static_cast<int32_t>(calibration_.secondary_stats().mean);
        set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                   reference_temperature_centi_c_,
                                   reference_humidity_centi_pct_,
                                   calibration_.stats().count +
                                       calibration_.secondary_stats().count,
                                   evidence_sequence_, 4U);
        if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      } else {
        // MQ calibration retains validated ADC evidence without inventing a
        // gas model, ppm conversion, R0, or alarm threshold.
        if (!active.present && !mq_engineering_set_) return false;
        if (pending_target_ == calibration::CalibrationTarget::Mq7) {
          acquisition::ActiveProfileSnapshot heater_snapshot{};
          const acquisition::ProfileKey heater_key{
              acquisition::ProfileKind::Heater,
              acquisition::ProfileTarget::Mq7};
          acquisition::Mq7HeaterProfile heater{};
          if (!profiles_->snapshot(heater_key, heater_snapshot) ||
              !heater_snapshot.present ||
              !codec.decode(heater_snapshot.encoded, heater) ||
              !heater.valid()) {
            return false;
          }
        }
        acquisition::MqAdcProfile profile{};
        if (active.present) {
          if (!codec.decode(active.encoded, profile)) return false;
        } else {
          profile.reference_mv = mq_reference_mv_;
          profile.full_scale_counts = mq_full_scale_counts_;
          profile.divider_numerator = mq_divider_numerator_;
          profile.divider_denominator = mq_divider_denominator_;
        }
        profile.manifest.kind = acquisition::ProfileKind::Adc;
        profile.manifest.schema_version = acquisition::kProfileSchemaVersion;
        profile.manifest.target_id = static_cast<uint32_t>(target);
        profile.manifest.validity = acquisition::ProfileValidity::Valid;
        profile.manifest.profile_version = version;
        set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                   static_cast<int64_t>(calibration_.stats().mean),
                                   0, calibration_.stats().count, evidence_sequence_, 5U);
        if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      }
      break;
  }
  proposal = CalibrationProposal{};
  proposal.present = true;
  proposal.key = key;
  proposal.encoded = encoded;
  proposal.active = active;
  proposal.created_at_us = now_us;
  proposal.evidence_sequence = evidence_sequence_;
  return true;
}

bool CommandRouter::calibration_proposal_current() const noexcept {
  if (!calibration_proposal_.present || profiles_ == nullptr ||
      calibration_proposal_.evidence_sequence != evidence_sequence_) {
    return false;
  }
  const auto target = static_cast<acquisition::ProfileTarget>(
      static_cast<uint8_t>(pending_target_) + 1U);
  const acquisition::ProfileKey expected = acquisition::default_profile_key(target);
  if (calibration_proposal_.key.kind != expected.kind ||
      calibration_proposal_.key.target != expected.target) return false;
  acquisition::ActiveProfileSnapshot active{};
  return profiles_->snapshot(calibration_proposal_.key, active) &&
         same_active_profile(calibration_proposal_.active, active);
}

bool CommandRouter::append_calibration_preview(MaintenanceResponse& response) const noexcept {
  if (!calibration_proposal_.present) return false;
  acquisition::ProfileCodec codec{};
  switch (calibration_proposal_.key.kind) {
    case acquisition::ProfileKind::Adc: {
      acquisition::MqAdcProfile profile{};
      return codec.decode(calibration_proposal_.encoded, profile) &&
             append_u32(response, "reference_mv=", profile.reference_mv) &&
             append_u32(response, "full_scale_counts=", profile.full_scale_counts) &&
             append_u32(response, "divider_numerator=", profile.divider_numerator) &&
             append_u32(response, "divider_denominator=", profile.divider_denominator) &&
             append_proposal_identity(response, calibration_proposal_.encoded,
                                     profile.manifest.profile_version,
                                     calibration_.stats().count + calibration_.secondary_stats().count,
                                     evidence_sequence_);
    }
    case acquisition::ProfileKind::Calibration:
      break;
    default:
      return false;
  }
  switch (pending_target_) {
    case calibration::CalibrationTarget::Hx711Organic:
    case calibration::CalibrationTarget::Hx711Inorganic:
    case calibration::CalibrationTarget::Hx711B3: {
      acquisition::Hx711Profile profile{};
      return codec.decode(calibration_proposal_.encoded, profile) &&
             append_i64(response, "tare_offset_counts=", profile.tare_offset_counts) &&
             append_float(response, "counts_per_kg=", profile.counts_per_kg) &&
             append_proposal_identity(response, calibration_proposal_.encoded,
                                     profile.manifest.profile_version,
                                     calibration_.stats().count + calibration_.secondary_stats().count,
                                     evidence_sequence_);
    }
    case calibration::CalibrationTarget::UltrasonicOrganic:
    case calibration::CalibrationTarget::UltrasonicInorganic:
    case calibration::CalibrationTarget::UltrasonicB3: {
      acquisition::UltrasonicProfile profile{};
      return codec.decode(calibration_proposal_.encoded, profile) &&
             append_float(response, "d_empty_mm=", profile.d_empty_mm) &&
             append_float(response, "d_full_mm=", profile.d_full_mm) &&
             append_float(response, "echo_mm_per_us=", profile.echo_mm_per_us) &&
             append_float(response, "overshoot_tolerance_mm=", profile.overshoot_tolerance_mm) &&
             append_u32(response, "echo_timeout_us=", profile.echo_timeout_us) &&
             append_u32(response, "inter_sensor_gap_us=", profile.inter_sensor_gap_us) &&
             append_proposal_identity(response, calibration_proposal_.encoded,
                                     profile.manifest.profile_version,
                                     calibration_.stats().count + calibration_.secondary_stats().count,
                                     evidence_sequence_);
    }
    case calibration::CalibrationTarget::Ds18b20B3: {
      acquisition::Ds18b20Profile profile{};
      return codec.decode(calibration_proposal_.encoded, profile) &&
             append_rom(response, profile.rom) &&
             append_u32(response, "resolution_bits=", profile.resolution_bits) &&
             append_i64(response, "offset_centi_c=", profile.offset_centi_c) &&
             append_proposal_identity(response, calibration_proposal_.encoded,
                                     profile.manifest.profile_version,
                                     calibration_.stats().count,
                                     evidence_sequence_);
    }
    case calibration::CalibrationTarget::Sht31_44:
    case calibration::CalibrationTarget::Sht31_45: {
      acquisition::Sht31Profile profile{};
      return codec.decode(calibration_proposal_.encoded, profile) &&
             append_u32(response, "address=", profile.address) &&
            append_i64(response, "temperature_offset_centi_c=", profile.temperature_offset_centi_c) &&
             append_i64(response, "humidity_offset_centi_pct=", profile.humidity_offset_centi_pct) &&
             append_proposal_identity(response, calibration_proposal_.encoded,
                                     profile.manifest.profile_version,
                                     calibration_.stats().count + calibration_.secondary_stats().count,
                                     evidence_sequence_);
    }
    default:
      return false;
  }
}

bool CommandRouter::commit_calibration_proposal() noexcept {
  if (!calibration_proposal_current()) return false;
  const CalibrationProposal proposal = calibration_proposal_;
  return profiles_->stage_import(proposal.key, proposal.encoded) &&
         profiles_->validate(proposal.key) && profiles_->preview(proposal.key) &&
         profiles_->commit(proposal.key);
}

}  // namespace sigap::maintenance
