#pragma once

// S2 typed normalized MQ-135 baseline profile (payload discriminator 2).
//
// The SGP1 envelope (22-byte header + payload + 32-byte SHA256) and the legacy
// payload-v1 gas profile bytes are unchanged. This profile stores a
// DIMENSIONLESS q0 baseline (q = Vc/AO - 1) instead of the legacy r0_ohm
// resistance, so an unknown module load RL is never required and no absolute
// ohms are invented. Byte layout is exactly S2_PROFILE_LAYOUT_RELEASE.json:
// 373 payload bytes, 427-byte envelope, little-endian integers and IEEE754
// binary32 floats.
//
// Provenance is explicit: every electrical quantity here is an OPERATOR
// DECLARATION for the requested input, not a verified physical measurement.
// Nothing in this file asserts accuracy, selectivity, clean air or that the
// declared divider matches the board.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "acquisition/mq135_gas.h"
#include "acquisition/mq135_nh3_curve.h"
#include "acquisition/profile_store.h"
#include "acquisition/profiles.h"
#include "telemetry/sha256.h"

namespace sigap::acquisition {

inline constexpr uint8_t kMq135NormalizedPayloadVersion{2U};
inline constexpr std::size_t kMq135NormalizedPayloadBytes{373U};
inline constexpr std::size_t kMq135NormalizedEnvelopeBytes{
    kProfileHeaderBytes + kMq135NormalizedPayloadBytes + kProfileHashBytes};
static_assert(kMq135NormalizedEnvelopeBytes == 427U,
              "released normalized envelope must be 427 bytes");
static_assert(kMq135NormalizedPayloadBytes <= kProfilePayloadBytes,
              "normalized payload must fit the SGP1 payload budget");

// Initial MQ-135 preparation attestation floor: strictly greater than 48 h.
inline constexpr uint64_t kMq135MinimumInitialPreheatUs{48ULL * 3600ULL * 1000000ULL};

enum class Mq135ReferenceKind : uint8_t {
  None = 0,
  Operational = 1,
  CleanAirConfirmed = 2,
};

enum class Mq135PreparationMethod : uint8_t {
  Unknown = 0,
  OperatorDeclared = 1,
};

enum class Mq135EstimationMethod : uint8_t {
  None = 0,
  PiecewiseLogLog = 1,
};

// Explicit operator-declared circuit. No RL member: the normalized response
// cancels the module load, and absolute resistance is out of scope here.
struct Mq135NormalizedElectrical final {
  uint32_t circuit_mv{0};
  uint32_t divider_top_ohm{0};
  uint32_t divider_bottom_ohm{0};
  Mq135Topology topology{Mq135Topology::Unknown};
  bool hardware_confirmed{false};

  bool valid() const noexcept {
    return hardware_confirmed && circuit_mv > 0U && divider_top_ohm > 0U &&
           divider_bottom_ohm > 0U &&
           (topology == Mq135Topology::DirectLoaded ||
            topology == Mq135Topology::Buffered);
  }

  // AO = pin / attenuation, attenuation = bottom / (top + bottom).
  double attenuation() const noexcept {
    const double divisor =
        static_cast<double>(divider_top_ohm) + static_cast<double>(divider_bottom_ohm);
    if (!(divisor > 0.0)) return 0.0;
    return static_cast<double>(divider_bottom_ohm) / divisor;
  }
};

struct Mq135NormalizedCriteria final {
  uint16_t samples{0};
  uint64_t min_duration_us{0};
  uint64_t max_age_us{0};
  float max_variation_percent{0};

  bool valid() const noexcept {
    return samples >= 3U && samples <= 64U && min_duration_us > 0U &&
           max_age_us > 0U && std::isfinite(max_variation_percent) &&
           max_variation_percent > 0.0F;
  }
};

struct Mq135NormalizedSummary final {
  bool ready{false};
  uint16_t count{0};
  uint16_t rejected{0};
  float q0{0};
  float minimum_q{0};
  float maximum_q{0};
  float variation_percent{0};
  uint64_t first_us{0};
  uint64_t last_us{0};
  config::ConfigHash evidence_hash{};
};

// Session-owned bounded window of admitted dimensionless q values. Same
// admission guards as the legacy reference window (monotone increasing
// sequence, non-decreasing time, bounded age) but the statistic is q, never
// ohms.
class Mq135NormalizedWindow final {
 public:
  bool configure(const Mq135NormalizedCriteria& criteria) noexcept {
    *this = Mq135NormalizedWindow{};
    criteria_ = criteria;
    configured_ = criteria.valid();
    return configured_;
  }

  bool observe(double q, uint64_t sequence, uint64_t observed_us,
               uint64_t now_us) noexcept {
    if (!configured_ || count_ >= criteria_.samples || !std::isfinite(q) ||
        q <= 0.0 || sequence == 0U || sequence <= sequence_ ||
        now_us < observed_us || now_us - observed_us > criteria_.max_age_us ||
        (count_ != 0U && observed_us <= last_us_)) {
      if (rejected_ != UINT16_MAX) ++rejected_;
      return false;
    }
    if (count_ == 0U) first_us_ = observed_us;
    last_us_ = observed_us;
    sequence_ = sequence;
    sequences_[count_] = sequence;
    times_[count_] = observed_us;
    values_[count_++] = q;
    return true;
  }

  Mq135NormalizedSummary summary() const noexcept {
    Mq135NormalizedSummary s{};
    s.count = count_;
    s.rejected = rejected_;
    s.first_us = first_us_;
    s.last_us = last_us_;
    if (count_ == 0U) return s;
    auto sorted = values_;
    std::sort(sorted.begin(), sorted.begin() + count_);
    s.minimum_q = static_cast<float>(sorted[0]);
    s.maximum_q = static_cast<float>(sorted[count_ - 1U]);
    s.q0 = count_ % 2U
               ? static_cast<float>(sorted[count_ / 2U])
               : static_cast<float>((sorted[count_ / 2U - 1U] + sorted[count_ / 2U]) / 2.0);
    s.variation_percent = static_cast<float>(
        100.0 * (sorted[count_ - 1U] - sorted[0]) /
        static_cast<double>(s.q0));
    std::array<char, 64U * 20U> canonical{};
    for (uint16_t i = 0; i < count_; ++i) {
      uint64_t bits{0U};
      std::memcpy(&bits, &values_[i], sizeof(values_[i]));
      for (unsigned j = 0; j < 8U; ++j) {
        canonical[i * 20U + j] = static_cast<char>(bits >> (j * 8U));
        canonical[i * 20U + 8U + j] = static_cast<char>(sequences_[i] >> (j * 8U));
        canonical[i * 20U + 12U + j] = static_cast<char>(times_[i] >> (j * 8U));
      }
    }
    s.evidence_hash = telemetry_contract::sha256(
        std::string_view(canonical.data(), count_ * 20U));
    s.ready = configured_ && rejected_ == 0U && count_ == criteria_.samples &&
              last_us_ - first_us_ >= criteria_.min_duration_us &&
              std::isfinite(s.variation_percent) &&
              s.variation_percent <= criteria_.max_variation_percent;
    return s;
  }

 private:
  Mq135NormalizedCriteria criteria_{};
  std::array<double, 64U> values_{};
  std::array<uint64_t, 64U> sequences_{};
  std::array<uint64_t, 64U> times_{};
  uint16_t count_{0U};
  uint16_t rejected_{0U};
  uint64_t sequence_{0U};
  uint64_t first_us_{0U};
  uint64_t last_us_{0U};
  bool configured_{false};
};

struct Mq135NormalizedProfile final {
  ProfileManifest manifest{};
  Mq135NormalizedElectrical electrical{};
  config::ConfigHash adc_hash{};
  uint32_t adc_version{0U};
  float q0{0.0F};
  float minimum_q{0.0F};
  float maximum_q{0.0F};
  uint32_t reference_pin_mv{0U};
  uint32_t reference_ao_mv{0U};
  float variation_percent{0.0F};
  uint16_t sample_count{0U};
  uint16_t rejected_count{0U};
  uint64_t first_us{0U};
  uint64_t last_us{0U};
  config::ConfigHash evidence_hash{};
  Mq135NormalizedCriteria criteria{};
  std::array<char, 32> hardware_identity{};
  std::array<char, 32> reference_environment{};
  bool temperature_present{false};
  int32_t temperature_centi_c{0};
  bool humidity_present{false};
  uint32_t humidity_centi_pct{0U};
  uint64_t heating_duration_us{0U};
  bool preparation_confirmed{false};
  Mq135PreparationMethod preparation_method{Mq135PreparationMethod::Unknown};
  Mq135ReferenceKind reference_kind{Mq135ReferenceKind::None};
  uint8_t adc_method{0U};  // MqAdcCalibrationMethod::EspAdcCalEfuseTpFit
  uint8_t resolution_bits{0U};
  uint8_t attenuation_db{0U};
  uint8_t adc_unit{0U};
  uint8_t adc_channel{0U};
  uint16_t curve_id{0U};
  uint16_t curve_version{0U};
  std::array<uint8_t, 32> curve_source_sha256{};
  Mq135EstimationMethod estimation_method{Mq135EstimationMethod::None};

  bool valid() const noexcept {
    if (!profile_validity_allows_conversion(manifest.validity) ||
        manifest.kind != ProfileKind::Calibration ||
        manifest.schema_version != 1U || manifest.profile_version == 0U ||
        manifest.target_id != static_cast<uint32_t>(ProfileTarget::Mq135)) {
      return false;
    }
    if (!electrical.valid() || adc_version == 0U ||
        std::all_of(adc_hash.begin(), adc_hash.end(), [](uint8_t x) { return x == 0U; })) {
      return false;
    }
    if (reference_kind == Mq135ReferenceKind::None) return false;
    if (!criteria.valid()) return false;
    if (sample_count < criteria.samples || rejected_count != 0U) return false;
    if (last_us < first_us || last_us - first_us < criteria.min_duration_us) return false;
    if (!std::isfinite(q0) || q0 <= 0.0F) return false;
    if (!std::isfinite(minimum_q) || minimum_q <= 0.0F) return false;
    if (!std::isfinite(maximum_q) || maximum_q < minimum_q) return false;
    if (q0 < minimum_q || q0 > maximum_q) return false;
    if (!std::isfinite(variation_percent) || variation_percent < 0.0F ||
        variation_percent > criteria.max_variation_percent) {
      return false;
    }
    if (reference_pin_mv == 0U || reference_ao_mv == 0U) return false;
    if (std::all_of(evidence_hash.begin(), evidence_hash.end(),
                    [](uint8_t x) { return x == 0U; })) {
      return false;
    }
    if (!terminated_label(hardware_identity) || !terminated_label(reference_environment)) {
      return false;
    }
    if (humidity_present && humidity_centi_pct > 10000U) return false;
    // Confirmed clean air requires an explicit, operator-declared initial
    // preheat strictly greater than 48 h. An OPERATIONAL baseline may keep a
    // valid q0/ratio with unconfirmed preparation; it can never be ppm-eligible.
    if (reference_kind == Mq135ReferenceKind::CleanAirConfirmed) {
      if (!preparation_confirmed ||
          preparation_method != Mq135PreparationMethod::OperatorDeclared ||
          heating_duration_us <= kMq135MinimumInitialPreheatUs) {
        return false;
      }
    }
    if (adc_method != static_cast<uint8_t>(MqAdcCalibrationMethod::EspAdcCalEfuseTpFit) ||
        resolution_bits != 12U || attenuation_db != 11U || adc_unit != 1U ||
        adc_channel != 4U) {
      return false;
    }
    if (curve_id != kMq135Nh3CurveId || curve_version != kMq135Nh3CurveVersion ||
        curve_source_sha256 != kMq135Nh3CurveSourceSha256 ||
        estimation_method != Mq135EstimationMethod::PiecewiseLogLog) {
      return false;
    }
    return manifest.provenance.present &&
           manifest.provenance.evidence_count == sample_count;
  }

 private:
  static bool terminated_label(const std::array<char, 32>& label) noexcept {
    return label[0] != '\0' && label.back() == '\0';
  }
};

// True only when the encoded payload is the normalized payload discriminator.
// Used by the shared codec to route validation without changing legacy bytes.
bool mq135_normalized_payload(const EncodedProfile& encoded) noexcept;

// Double-precision runtime response. The persisted q0 is binary32 profile
// precision; the current q and the estimator ratio are computed in double and
// are never routed back through the float ratio helper. Both reject
// non-finite/non-positive input and out-of-range quotients instead of clamping
// or reusing a last-good value.
inline bool mq135_normalized_response_double(double circuit_mv, double ao_mv,
                                             double& q) noexcept {
  q = 0.0;
  if (!std::isfinite(circuit_mv) || circuit_mv <= 0.0) return false;
  if (!std::isfinite(ao_mv) || ao_mv <= 0.0 || ao_mv >= circuit_mv) return false;
  const double value = circuit_mv / ao_mv - 1.0;
  if (!std::isfinite(value) || value <= 0.0) return false;
  q = value;
  return true;
}

inline bool mq135_normalized_ratio_double(double q, double q0,
                                          double& ratio) noexcept {
  ratio = 0.0;
  if (!std::isfinite(q) || q <= 0.0 || !std::isfinite(q0) || q0 <= 0.0) return false;
  const double value = q / q0;
  if (!std::isfinite(value) || value <= 0.0) return false;
  ratio = value;
  return true;
}

bool encode_mq135_normalized_profile(const Mq135NormalizedProfile& profile,
                                     EncodedProfile& output) noexcept;
bool decode_mq135_normalized_profile(const EncodedProfile& encoded,
                                     Mq135NormalizedProfile& output) noexcept;

// Immutable MQG2 control-result evidence. Every quantity carries an explicit
// presence flag so the serializer emits null instead of a fabricated zero. The
// snapshot is what a durable replay caches: it is never retimestamped and never
// recomputed from a newer sample.
struct Mq135NormalizedDiagnostic final {
  bool present{false};

  // root
  std::array<char, 32> quality{};
  std::array<char, 32> reason{};

  // adc
  bool raw_present{false};
  uint16_t raw_counts{0U};
  bool adc_available{false};
  bool pin_present{false};
  float pin_mv{0.0F};
  bool ao_present{false};
  float ao_mv{0.0F};
  bool same_sample{false};
  std::array<char, 32> adc_failure{};
  uint8_t resolution_bits{0U};
  uint8_t attenuation_db{0U};
  uint8_t adc_unit{0U};
  uint8_t adc_channel{0U};

  // electrical
  bool circuit_present{false};
  uint32_t circuit_mv{0U};
  uint32_t divider_top_ohm{0U};
  uint32_t divider_bottom_ohm{0U};
  Mq135Topology topology{Mq135Topology::Unknown};
  bool hardware_confirmed{false};
  std::array<char, 32> hardware_identity{};

  // response
  bool q_present{false};
  double q{0.0};
  bool q0_present{false};
  float q0{0.0F};
  bool ratio_present{false};
  double ratio{0.0};
  Mq135ReferenceKind reference_kind{Mq135ReferenceKind::None};
  bool baseline_valid{false};

  // nh3
  bool ppm_present{false};
  double ppm{0.0};
  // Longest documented status label is ADC_CALIBRATION_UNAVAILABLE (27 chars).
  // A 24-byte field silently truncated it to ADC_CALIBRATION_UNAVAIL, which is
  // not a contract enum value, so the field is sized to hold every label.
  std::array<char, 32> nh3_status{};
  bool curve_present{false};
  uint16_t curve_id{0U};
  uint16_t curve_version{0U};
  std::array<uint8_t, 32> curve_source_sha256{};

  // profiles
  std::array<char, 16> profile_state{};
  ProfileRef profile{};
  ProfileRef previous_profile{};
  ProfileRef adc_profile{};

  // source
  bool source_present{false};
  BootId source_boot_id{};
  uint64_t source_sequence{0U};
  uint64_t source_us{0U};
  uint64_t response_us{0U};
  bool clock_known{false};
  bool age_known{false};
  bool fresh{false};
  uint64_t max_age_us{0U};
  std::array<char, 32> source_quality{};

  // reference
  std::array<char, 32> reference_environment{};
  bool temperature_present{false};
  int32_t temperature_centi_c{0};
  bool humidity_present{false};
  uint32_t humidity_centi_pct{0U};
  bool conditions_declared{false};
  uint64_t heating_duration_us{0U};
  bool preparation_confirmed{false};
  Mq135PreparationMethod preparation_method{Mq135PreparationMethod::Unknown};
  uint16_t sample_count{0U};
  float variation_percent{0.0F};
  RecordedAt calibration_recorded_at{};
  TimeQuality calibration_time_quality{TimeQuality::Unsynced};
};

}  // namespace sigap::acquisition
