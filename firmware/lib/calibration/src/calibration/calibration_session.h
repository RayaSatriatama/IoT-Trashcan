#pragma once

#include <cstdint>
#include <variant>

#include "acquisition/acquisition_types.h"
#include "acquisition/heater.h"
#include "acquisition/profiles.h"

namespace sigap::calibration {

enum class CalibrationState : uint8_t {
  Idle,
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
  Internal,
};

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
};

struct UltrasonicTrial final {
  float distance_mm{0.0F};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
};

struct MqTrial final {
  uint16_t adc_counts{0U};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool heater_ready{true};
};

struct Ds18b20Trial final {
  int32_t temperature_centi_c{0};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool raw_present{false};
  bool rom_match{false};
  bool conversion_complete{false};
  bool crc_valid{false};
};

struct Sht31Trial final {
  int32_t temperature_centi_c{0};
  int32_t humidity_centi_pct{0};
  acquisition::SampleQuality quality{acquisition::SampleQuality::Timeout};
  bool raw_present{false};
  bool temperature_crc_valid{false};
  bool humidity_crc_valid{false};
};

using TrialObservation = std::variant<Hx711Trial, UltrasonicTrial, MqTrial,
                                      Ds18b20Trial, Sht31Trial>;

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

  bool start(CalibrationTarget target, const CaptureCriteria& criteria,
             uint64_t now_us) noexcept;
  bool observe(const TrialObservation& trial) noexcept;
  bool validate() noexcept;
  bool preview() noexcept;
  bool request_commit() noexcept;
  bool complete_commit(bool committed) noexcept;
  bool cancel() noexcept;
  void fail(CalibrationFailure failure) noexcept;

  CalibrationState state() const noexcept { return state_; }
  CalibrationTarget target() const noexcept { return target_; }
  CalibrationFailure failure() const noexcept { return failure_; }
  const CaptureStats& stats() const noexcept { return primary_; }
  const CaptureStats& secondary_stats() const noexcept { return secondary_; }
  const ValidationResult& validation() const noexcept { return validation_; }
  bool commit_requested() const noexcept { return state_ == CalibrationState::Commit; }
  uint64_t started_at_us() const noexcept { return started_at_us_; }

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
};

}  // namespace sigap::calibration
