#pragma once

#include <cstdint>
#include <array>
#include <cmath>
#include <variant>

#include "acquisition/acquisition_types.h"
#include "acquisition/record.h"
#include "acquisition/heater.h"
#include "acquisition/profiles.h"

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
};

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

using CaptureCriteria = std::variant<Hx711CaptureCriteria,
                                     DistanceCaptureCriteria,
                                     MqCaptureCriteria,
                                     Ds18b20CaptureCriteria,
                                     Sht31CaptureCriteria>;

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

class ICalibrationEvidenceSource {
 public:
  virtual ~ICalibrationEvidenceSource() = default;
  virtual bool latest(CalibrationTarget target, uint64_t after_sequence,
                      TrialObservation& output,
                      uint64_t& observed_sequence) const noexcept = 0;
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
      CalibrationTarget target, float echo_mm_per_us) noexcept;
  bool latest(CalibrationTarget target, uint64_t after_sequence,
              TrialObservation& output,
              uint64_t& observed_sequence) const noexcept override;

 private:
  static constexpr std::size_t kTargetCount{13U};
  std::array<TrialObservation, kTargetCount> observations_{};
  std::array<uint64_t, kTargetCount> sequences_{};
  std::array<bool, kTargetCount> present_{};
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

 private:
  bool criteria_valid(const CaptureCriteria& criteria) const noexcept;
  bool criteria_matches_target(const CaptureCriteria& criteria) const noexcept;
  bool required_samples_reached() const noexcept;
  bool observe_hx711(const Hx711Trial& trial) noexcept;
  bool observe_ultrasonic(const UltrasonicTrial& trial) noexcept;
  bool observe_mq(const MqTrial& trial) noexcept;
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
};

}  // namespace sigap::calibration
