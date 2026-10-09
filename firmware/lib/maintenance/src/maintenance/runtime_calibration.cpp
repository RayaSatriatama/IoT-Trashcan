#include "maintenance/runtime.h"

#include "acquisition/target_protocol.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sigap::maintenance {
namespace {

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

}  // namespace

bool CommandRouter::build_and_stage_calibration_profile(uint64_t now_us) noexcept {
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
  return profiles_->stage_import(key, encoded) && profiles_->validate(key) &&
         profiles_->preview(key) && profiles_->commit(key);
}

}  // namespace sigap::maintenance
