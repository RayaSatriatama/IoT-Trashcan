#pragma once

#include <cstdint>
#include <array>
#include <cmath>
#include <variant>

#include "acquisition/acquisition_types.h"
#include "acquisition/record.h"
#include "acquisition/heater.h"
#include "acquisition/profiles.h"
#include "acquisition/mq135_gas.h"
#include "acquisition/mq135_normalized.h"

namespace sigap::acquisition {
struct TargetProfileSet;
}

namespace sigap::calibration {

enum class CalibrationState : uint8_t {
  Idle,
  Configuring,
  Capture,
  Validate,
  Preview,
  Commit,
  Failed,
};

enum class CalibrationTarget : uint8_t {
  Hx711Organic,
  Hx711Inorganic,
  Hx711B3,
  UltrasonicOrganic,
  UltrasonicInorganic,
  UltrasonicB3,
  Mq4,
  Mq135,
  Mq7,
  Mq8,
  Ds18b20B3,
  Sht31_44,
  Sht31_45,
  Mq135Gas,
};

inline acquisition::ProfileKey calibration_profile_key(CalibrationTarget target) noexcept {
  if (target == CalibrationTarget::Mq135Gas) return acquisition::kMq135GasKey;
  return acquisition::default_profile_key(static_cast<acquisition::ProfileTarget>(static_cast<uint8_t>(target) + 1U));
}

enum class CalibrationFailure : uint8_t {
  None,
  InvalidCriteria,
  SensorDisconnected,
  CommitRejected,
  Timeout,
  Internal,
};

enum class CalibrationAnchor : uint8_t { Primary, Tare, Reference, Empty, Full };

enum class ValidationCode : uint8_t {
  Valid,
  NoEvidence,
  InsufficientSamples,
  Unstable,
  InvalidProfile,
  InvalidIdentity,
  InvalidCriteria,
};

struct CaptureStats final {
  uint32_t count{0U};
  uint32_t quality_failures{0U};
  double minimum{0.0};
  double maximum{0.0};
  double mean{0.0};

  void observe(double value) noexcept;
  double span() const noexcept;
};

struct Hx711CaptureCriteria final {
  uint16_t required_samples{0U};
  int64_t max_span_counts{0};
};

struct DistanceCaptureCriteria final {
  uint16_t required_samples{0U};
  float max_span_mm{0.0F};
};

struct MqCaptureCriteria final {
  uint16_t required_samples{0U};
  uint16_t max_span_counts{0U};
  bool require_low_detect{false};
};

struct Ds18b20CaptureCriteria final {
  uint16_t required_samples{0U};
  int32_t max_span_centi_c{0};
  bool require_rom_match{true};
};

struct Sht31CaptureCriteria final {
  uint16_t required_samples{0U};
  int32_t max_temperature_span_centi_c{0};
  int32_t max_humidity_span_centi_pct{0};
};

struct Mq135GasCaptureCriteria final {
  uint16_t required_samples{0};
  acquisition::Mq135GasProfile profile{};
  // Injected by the device from ProfileManager, not a wire-supplied coefficient.
  acquisition::MqAdcProfile adc_profile{};
  bool reference_confirmed{false};
  bool valid() const noexcept {
    return reference_confirmed && required_samples == profile.criteria.samples &&
      profile.criteria.valid() && profile.electrical.valid() &&
      profile.hardware_identity[0] && profile.hardware_identity.back() == '\0' &&
      profile.reference_environment[0] && profile.reference_environment.back() == '\0' &&
      profile.preparation_confirmed && profile.heating_duration_us > 0 &&
      profile.humidity_centi_percent <= 10000 && adc_profile.valid_for_conversion() &&
      adc_profile.manifest.kind == acquisition::ProfileKind::Adc &&
      adc_profile.manifest.target_id == 8 && adc_profile.manifest.profile_version > 0 &&
      adc_profile.manifest.provenance.present && adc_profile.manifest.provenance.evidence_count > 0 &&
      std::any_of(adc_profile.manifest.hash.begin(), adc_profile.manifest.hash.end(), [](uint8_t x) { return x != 0; });
  }
};

inline bool gas_criteria_configuration_equal(const Mq135GasCaptureCriteria& a,
                                              const Mq135GasCaptureCriteria& b) noexcept {
  const auto& x = a.profile; const auto& y = b.profile;
  return a.required_samples == b.required_samples && a.reference_confirmed == b.reference_confirmed &&
    x.electrical.circuit_mv == y.electrical.circuit_mv &&
    x.electrical.module_load_ohm == y.electrical.module_load_ohm &&
    x.electrical.divider_top_ohm == y.electrical.divider_top_ohm &&
    x.electrical.divider_bottom_ohm == y.electrical.divider_bottom_ohm &&
    x.electrical.topology == y.electrical.topology && x.electrical.verified == y.electrical.verified &&
    x.criteria.samples == y.criteria.samples && x.criteria.min_duration_us == y.criteria.min_duration_us &&
    x.criteria.max_age_us == y.criteria.max_age_us &&
    x.criteria.max_variation_percent == y.criteria.max_variation_percent &&
    x.hardware_identity == y.hardware_identity && x.reference_environment == y.reference_environment &&
    x.temperature_centi_c == y.temperature_centi_c && x.humidity_centi_percent == y.humidity_centi_percent &&
    x.heating_duration_us == y.heating_duration_us && x.preparation_confirmed == y.preparation_confirmed;
}

// Normalized (dimensionless q0) reference capture criteria. Same session and
// controller as the legacy gas mode; the discriminator is the criteria type,
// selected by the wire `response_mode=normalized` key.
struct Mq135NormalizedCaptureCriteria final {
  uint16_t required_samples{0};
  acquisition::Mq135NormalizedElectrical electrical{};
  acquisition::Mq135NormalizedCriteria criteria{};
  // Injected by the device from ProfileManager, never a wire coefficient.
  acquisition::MqAdcProfile adc_profile{};
  bool reference_confirmed{false};
  acquisition::Mq135ReferenceKind reference_kind{acquisition::Mq135ReferenceKind::None};
  std::array<char, 32> hardware_identity{};
  std::array<char, 32> reference_environment{};
  bool temperature_present{false};
  int32_t temperature_centi_c{0};
  bool humidity_present{false};
  uint32_t humidity_centi_pct{0U};
  uint64_t heating_duration_us{0U};
  bool preparation_confirmed{false};
  acquisition::Mq135PreparationMethod preparation_method{
      acquisition::Mq135PreparationMethod::Unknown};

  bool valid() const noexcept {
    if (!reference_confirmed || required_samples != criteria.samples ||
        !criteria.valid() || !electrical.valid()) {
      return false;
    }
    if (hardware_identity[0] == '\0' || hardware_identity.back() != '\0' ||
        reference_environment[0] == '\0' || reference_environment.back() != '\0') {
      return false;
    }
    if (humidity_present && humidity_centi_pct > 10000U) return false;
    if (reference_kind == acquisition::Mq135ReferenceKind::None) return false;
    if (reference_kind == acquisition::Mq135ReferenceKind::CleanAirConfirmed) {
      if (!preparation_confirmed ||
          preparation_method != acquisition::Mq135PreparationMethod::OperatorDeclared ||
          heating_duration_us <= acquisition::kMq135MinimumInitialPreheatUs) {
        return false;
      }
    }
    if (preparation_confirmed &&
        preparation_method != acquisition::Mq135PreparationMethod::OperatorDeclared) {
      return false;
    }
    const auto& adc = adc_profile;
    return adc.valid_for_conversion() &&
           adc.manifest.kind == acquisition::ProfileKind::Adc &&
           adc.manifest.target_id == 8 && adc.manifest.profile_version > 0U &&
           adc.manifest.provenance.present &&
           adc.manifest.provenance.evidence_count > 0U &&
           std::any_of(adc.manifest.hash.begin(), adc.manifest.hash.end(),
                       [](uint8_t x) { return x != 0U; });
  }
};

inline bool normalized_criteria_configuration_equal(
    const Mq135NormalizedCaptureCriteria& a,
    const Mq135NormalizedCaptureCriteria& b) noexcept {
  return a.required_samples == b.required_samples &&
         a.reference_confirmed == b.reference_confirmed &&
         a.reference_kind == b.reference_kind &&
         a.electrical.circuit_mv == b.electrical.circuit_mv &&
         a.electrical.divider_top_ohm == b.electrical.divider_top_ohm &&
         a.electrical.divider_bottom_ohm == b.electrical.divider_bottom_ohm &&
         a.electrical.topology == b.electrical.topology &&
         a.electrical.hardware_confirmed == b.electrical.hardware_confirmed &&
         a.criteria.samples == b.criteria.samples &&
         a.criteria.min_duration_us == b.criteria.min_duration_us &&
         a.criteria.max_age_us == b.criteria.max_age_us &&
         a.criteria.max_variation_percent == b.criteria.max_variation_percent &&
         a.hardware_identity == b.hardware_identity &&
         a.reference_environment == b.reference_environment &&
         a.temperature_present == b.temperature_present &&
         a.temperature_centi_c == b.temperature_centi_c &&
         a.humidity_present == b.humidity_present &&
         a.humidity_centi_pct == b.humidity_centi_pct &&
         a.heating_duration_us == b.heating_duration_us &&
         a.preparation_confirmed == b.preparation_confirmed &&
         a.preparation_method == b.preparation_method &&
         a.adc_profile.manifest.profile_version ==
             b.adc_profile.manifest.profile_version &&
         a.adc_profile.manifest.hash == b.adc_profile.manifest.hash;
}

using CaptureCriteria = std::variant<Hx711CaptureCriteria,
                                     DistanceCaptureCriteria,
                                     MqCaptureCriteria,
                                     Ds18b20CaptureCriteria,
                                     Sht31CaptureCriteria,
                                     Mq135GasCaptureCriteria,
                                     Mq135NormalizedCaptureCriteria>;

struct Hx711Trial final {
  int64_t counts{0};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool raw_present{true};
  bool identity_valid{true};
};

struct UltrasonicTrial final {
  float distance_mm{0.0F};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool conversion_valid{true};
  bool identity_valid{true};
};

struct MqTrial final {
  uint16_t adc_counts{0U};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool heater_ready{true};
  bool raw_present{true};
  bool identity_valid{true};
  uint64_t observed_at_us{0}, source_sequence{0};
  acquisition::ProfileRef source_profile{};
  acquisition::BootId source_boot_id{};
  acquisition::RecordedAt source_recorded_at{};
  acquisition::TimeQuality source_time_quality{acquisition::TimeQuality::Unsynced};
  // Appended after every legacy field so existing aggregate initializers keep
  // their meaning. Carried verbatim from the same SampleRecord; a missing
  // calibrated field stays unavailable rather than becoming a nominal voltage.
  acquisition::MqAdcCalibratedVoltage calibrated{};
};

struct Ds18b20Trial final {
  int32_t temperature_centi_c{0};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool raw_present{false};
  bool rom_match{false};
  bool conversion_complete{false};
  bool crc_valid{false};
  bool wire_evidence{false};
  std::array<uint8_t, 8> rom{};
  std::array<uint8_t, 9> scratchpad{};
  uint8_t resolution_bits{0U};
};

struct Sht31Trial final {
  int32_t temperature_centi_c{0};
  int32_t humidity_centi_pct{0};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool raw_present{false};
  bool temperature_crc_valid{false};
  bool humidity_crc_valid{false};
  bool identity_valid{true};
};

using TrialObservation = std::variant<Hx711Trial, UltrasonicTrial, MqTrial,
                                      Ds18b20Trial, Sht31Trial>;

struct DiagnosticEvidence final {
  bool available{false};
  acquisition::SensorId sensor{acquisition::SensorId::Mq4};
  acquisition::StreamId stream{acquisition::StreamId::Organic};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  acquisition::SampleReason reason{acquisition::SampleReason::None};
  bool raw_present{false};
  bool identity_valid{false};
  bool crc_valid{false};
  bool conversion_complete{false};
  bool temperature_crc_valid{false};
  bool humidity_crc_valid{false};
  bool heater_ready{false};
  bool wire_evidence{false};
  uint64_t observed_at_us{0U};
  uint32_t accepted_count{0U};
  uint32_t rejected_count{0U};
};

class ICalibrationEvidenceSource {
 public:
  virtual ~ICalibrationEvidenceSource() = default;
  virtual bool latest(CalibrationTarget target, uint64_t after_sequence,
                      TrialObservation& output,
                      uint64_t& observed_sequence) const noexcept = 0;
  virtual bool diagnostic(CalibrationTarget,
                          DiagnosticEvidence&) const noexcept { return false; }
  // Optional session-owned engineering setup for first-time ultrasonic
  // calibration. Sources that do not expose this setting return false.
  virtual bool set_ultrasonic_engineering_conversion(
      CalibrationTarget, float) noexcept { return false; }
};

// First-time ultrasonic setup may provide only the validated engineering
// echo-to-distance conversion. Timing and fill calibration remain profile data
// and are not inferred from this bounded cache setting.
struct UltrasonicEngineeringConversion final {
  float echo_mm_per_us{0.0F};

  bool valid() const noexcept {
    return std::isfinite(echo_mm_per_us) && echo_mm_per_us > 0.0F;
  }
};

// A single latest observation per channel keeps calibration independent from
// the future logger handoff and bounds RAM usage.
class CalibrationEvidenceCache final
    : public acquisition::ISampleObserver,
      public ICalibrationEvidenceSource {
 public:
  void observe(const acquisition::SampleRecord& sample) noexcept override;
  void set_profiles(const acquisition::TargetProfileSet* profiles) noexcept {
    profiles_ = profiles;
  }
  bool set_ultrasonic_engineering_conversion(
      CalibrationTarget target, float echo_mm_per_us) noexcept override;
  bool latest(CalibrationTarget target, uint64_t after_sequence,
              TrialObservation& output,
              uint64_t& observed_sequence) const noexcept override;
  bool diagnostic(CalibrationTarget target,
                  DiagnosticEvidence& output) const noexcept override;

 private:
  static constexpr std::size_t kTargetCount{13U};
  std::array<TrialObservation, kTargetCount> observations_{};
  std::array<uint64_t, kTargetCount> sequences_{};
  std::array<bool, kTargetCount> present_{};
  std::array<DiagnosticEvidence, kTargetCount> diagnostics_{};
  std::array<UltrasonicEngineeringConversion, 3U>
      ultrasonic_engineering_conversion_{};
  const acquisition::TargetProfileSet* profiles_{nullptr};
};

struct ValidationResult final {
  ValidationCode code{ValidationCode::NoEvidence};
  CaptureStats primary{};
  CaptureStats secondary{};

  bool valid() const noexcept { return code == ValidationCode::Valid; }
};

enum class ProfileValidationCode : uint8_t {
  Valid,
  Missing,
  InvalidIdentity,
  InvalidRange,
};

struct ProfileValidationResult final {
  ProfileValidationCode code{ProfileValidationCode::Missing};

  bool valid() const noexcept { return code == ProfileValidationCode::Valid; }
};

ProfileValidationResult validate_hx711_profile(
    const acquisition::Hx711Profile& profile) noexcept;
ProfileValidationResult validate_ultrasonic_profile(
    const acquisition::UltrasonicProfile& profile) noexcept;
ProfileValidationResult validate_mq_profile(
    const acquisition::MqAdcProfile& profile,
    acquisition::SensorId sensor) noexcept;
ProfileValidationResult validate_mq7_profile(
    const acquisition::Mq7HeaterProfile& profile) noexcept;
ProfileValidationResult validate_ds18b20_profile(
    const acquisition::Ds18b20Profile& profile) noexcept;
ProfileValidationResult validate_sht31_profile(
    const acquisition::Sht31Profile& profile,
    uint8_t address) noexcept;

class CalibrationSession final {
 public:
  CalibrationSession() noexcept = default;

  bool begin(CalibrationTarget target, uint64_t now_us) noexcept;
  bool start(CalibrationTarget target, const CaptureCriteria& criteria,
             uint64_t now_us) noexcept;
  bool configure(const CaptureCriteria& criteria, uint64_t now_us) noexcept;
  bool observe(const TrialObservation& trial) noexcept;
  // Declared environment may arrive before the first admitted sample; the
  // window/criteria identity otherwise stays immutable for the session.
  bool update_normalized_environment(bool temperature_present,
                                     int32_t temperature_centi_c,
                                     bool humidity_present,
                                     uint32_t humidity_centi_pct) noexcept;
  bool observe_anchor(const TrialObservation& trial,
                      CalibrationAnchor anchor) noexcept;
  void touch(uint64_t now_us) noexcept;
  bool tick(uint64_t now_us) noexcept;
  bool validate() noexcept;
  bool preview() noexcept;
  bool request_commit() noexcept;
  bool complete_commit(bool committed) noexcept;
  bool cancel() noexcept;
  void abort() noexcept { reset(); }
  void fail(CalibrationFailure failure) noexcept;

  CalibrationState state() const noexcept { return state_; }
  CalibrationTarget target() const noexcept { return target_; }
  CalibrationFailure failure() const noexcept { return failure_; }
  const CaptureStats& stats() const noexcept { return primary_; }
  const CaptureStats& secondary_stats() const noexcept { return secondary_; }
  const ValidationResult& validation() const noexcept { return validation_; }
  bool commit_requested() const noexcept { return state_ == CalibrationState::Commit; }
  uint64_t started_at_us() const noexcept { return started_at_us_; }
  uint64_t last_activity_us() const noexcept { return last_activity_us_; }
  static constexpr uint64_t kSessionTimeoutUs{300000000ULL};
  acquisition::Mq135ReferenceSummary gas_summary() const noexcept { return gas_window_.summary(); }
  const Mq135GasCaptureCriteria* gas_criteria() const noexcept { return std::get_if<Mq135GasCaptureCriteria>(&criteria_); }
  const MqTrial& gas_last_observation() const noexcept { return gas_last_; }
  acquisition::Mq135NormalizedSummary normalized_summary() const noexcept {
    return normalized_window_.summary();
  }
  const Mq135NormalizedCaptureCriteria* normalized_criteria() const noexcept {
    return std::get_if<Mq135NormalizedCaptureCriteria>(&criteria_);
  }
  const MqTrial& normalized_last_observation() const noexcept {
    return normalized_last_;
  }

 private:
  bool criteria_valid(const CaptureCriteria& criteria) const noexcept;
  bool criteria_matches_target(const CaptureCriteria& criteria) const noexcept;
  bool required_samples_reached() const noexcept;
  bool observe_hx711(const Hx711Trial& trial) noexcept;
  bool observe_ultrasonic(const UltrasonicTrial& trial) noexcept;
  bool observe_mq(const MqTrial& trial) noexcept;
  bool observe_gas(const MqTrial& trial) noexcept;
  bool observe_normalized(const MqTrial& trial) noexcept;
  bool observe_ds18b20(const Ds18b20Trial& trial) noexcept;
  bool observe_sht31(const Sht31Trial& trial) noexcept;
  bool valid_span(double span, double maximum) const noexcept;
  void reset() noexcept;

  CalibrationState state_{CalibrationState::Idle};
  CalibrationTarget target_{CalibrationTarget::Hx711Organic};
  CaptureCriteria criteria_{Hx711CaptureCriteria{}};
  CaptureStats primary_{};
  CaptureStats secondary_{};
  ValidationResult validation_{};
  CalibrationFailure failure_{CalibrationFailure::None};
  uint64_t started_at_us_{0U};
  uint64_t last_activity_us_{0U};
  bool secondary_required_{false};
  acquisition::Mq135ReferenceWindow gas_window_{};
  MqTrial gas_last_{};
  acquisition::Mq135NormalizedWindow normalized_window_{};
  MqTrial normalized_last_{};
};

}  // namespace sigap::calibration
