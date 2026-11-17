#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include "acquisition/profiles.h"
#include "telemetry/sha256.h"

namespace sigap::acquisition {

// No topology, resistor, preparation or concentration defaults are implied.
enum class Mq135Topology : uint8_t { Unknown, DirectLoaded, Buffered };
enum class Mq135GasReason : uint8_t {
  None, ElectricalUnverified, AdcInvalid, DividerMismatch, Saturated,
  VoltageInvalid, SourceInvalid, Stale, ReferenceMissing, ReferenceUnstable,
  IdentityChanged, Preparing, Nh3Uncalibrated
};
struct Mq135Electrical final {
  float circuit_mv{0};
  float module_load_ohm{0};
  float divider_top_ohm{0};
  float divider_bottom_ohm{0};
  Mq135Topology topology{Mq135Topology::Unknown};
  bool verified{false};
  bool valid() const noexcept {
    return verified && std::isfinite(circuit_mv) && circuit_mv > 0 &&
      std::isfinite(module_load_ohm) && module_load_ohm > 0 &&
      std::isfinite(divider_top_ohm) && divider_top_ohm > 0 &&
      std::isfinite(divider_bottom_ohm) && divider_bottom_ohm > 0 &&
      (topology == Mq135Topology::DirectLoaded || topology == Mq135Topology::Buffered);
  }
};
struct Mq135Resistance final {
  Mq135GasReason reason{Mq135GasReason::SourceInvalid};
  float adc_pin_mv{0};
  float ao_mv{0};
  float rs_ohm{0};
  bool ready() const noexcept { return reason == Mq135GasReason::None; }
};
inline Mq135Resistance mq135_resistance(uint32_t counts, const MqAdcProfile& adc,
                                       const Mq135Electrical& e) noexcept {
  Mq135Resistance r{};
  if (!e.valid()) { r.reason = Mq135GasReason::ElectricalUnverified; return r; }
  if (!adc.valid_for_conversion() || adc.manifest.kind != ProfileKind::Adc ||
      adc.manifest.target_id != static_cast<uint32_t>(ProfileTarget::Mq135) ||
      adc.manifest.profile_version == 0 || adc.divider_numerator > adc.divider_denominator) {
    r.reason = Mq135GasReason::AdcInvalid; return r;
  }
  if (counts == 0 || counts >= adc.full_scale_counts) {
    r.reason = Mq135GasReason::Saturated; return r;
  }
  const double divisor = static_cast<double>(e.divider_top_ohm) + e.divider_bottom_ohm;
  const double attenuation = e.divider_bottom_ohm / divisor;
  const double configured = static_cast<double>(adc.divider_numerator) / adc.divider_denominator;
  // Numerical identity tolerance, not a scientific stability/accuracy threshold.
  if (!std::isfinite(attenuation) || std::abs(attenuation - configured) > 1e-6) {
    r.reason = Mq135GasReason::DividerMismatch; return r;
  }
  const double pin = static_cast<double>(counts) * adc.reference_mv / adc.full_scale_counts;
  const double ao = pin / attenuation;
  const double rl = e.topology == Mq135Topology::DirectLoaded
    ? 1.0 / (1.0 / e.module_load_ohm + 1.0 / divisor) : e.module_load_ohm;
  const double rs = rl * (static_cast<double>(e.circuit_mv) / ao - 1.0);
  if (!std::isfinite(ao) || ao <= 0 || ao >= e.circuit_mv ||
      !std::isfinite(rs) || rs <= 0 || !std::isfinite(static_cast<float>(rs)) ||
      static_cast<float>(rs) <= 0) {
    r.reason = Mq135GasReason::VoltageInvalid; return r;
  }
  r.adc_pin_mv = static_cast<float>(pin); r.ao_mv = static_cast<float>(ao);
  r.rs_ohm = static_cast<float>(rs); r.reason = Mq135GasReason::None;
  return r;
}
struct Mq135ReferenceCriteria final {
  uint16_t samples{0};
  uint64_t min_duration_us{0};
  uint64_t max_age_us{0};
  float max_variation_percent{0};
  bool valid() const noexcept {
    return samples >= 3 && samples <= 64 && min_duration_us > 0 && max_age_us > 0 &&
      std::isfinite(max_variation_percent) && max_variation_percent > 0;
  }
};
struct Mq135ReferenceSummary final {
  bool ready{false};
  uint16_t count{0};
  uint16_t rejected{0};
  float median_ohm{0};
  float minimum_ohm{0};
  float maximum_ohm{0};
  float variation_percent{0};
  uint64_t first_us{0};
  uint64_t last_us{0};
  config::ConfigHash evidence_hash{};
};
// Session-owned bounded evidence, never a historical transport backlog.
class Mq135ReferenceWindow final {
 public:
  bool configure(const Mq135ReferenceCriteria& criteria) noexcept {
    *this = Mq135ReferenceWindow{};
    criteria_ = criteria; configured_ = criteria.valid(); return configured_;
  }
  bool observe(float rs, uint64_t sequence, uint64_t observed_us, uint64_t now_us) noexcept {
    if (!configured_ || count_ >= criteria_.samples || !std::isfinite(rs) || rs <= 0 ||
        sequence == 0 || sequence <= sequence_ || now_us < observed_us ||
        now_us - observed_us > criteria_.max_age_us ||
        (count_ && observed_us <= last_us_)) {
      if (rejected_ != UINT16_MAX) ++rejected_;
      return false;
    }
    if (!count_) first_us_ = observed_us;
    last_us_ = observed_us; sequence_ = sequence;
    sequences_[count_] = sequence; times_[count_] = observed_us; values_[count_++] = rs;
    return true;
  }
  Mq135ReferenceSummary summary() const noexcept {
    Mq135ReferenceSummary s{}; s.count = count_; s.rejected = rejected_;
    s.first_us = first_us_; s.last_us = last_us_;
    if (!count_) return s;
    auto sorted = values_; std::sort(sorted.begin(), sorted.begin() + count_);
    s.minimum_ohm = sorted[0]; s.maximum_ohm = sorted[count_ - 1];
    s.median_ohm = count_ % 2 ? sorted[count_ / 2]
      : static_cast<float>((static_cast<double>(sorted[count_ / 2 - 1]) + sorted[count_ / 2]) / 2);
    s.variation_percent = static_cast<float>(
      100.0 * (static_cast<double>(s.maximum_ohm) - s.minimum_ohm) / s.median_ohm);
    std::array<char, 64 * 20> canonical{};
    for (uint16_t i = 0; i < count_; ++i) {
      uint32_t bits{}; std::memcpy(&bits, &values_[i], sizeof(bits));
      for (unsigned j = 0; j < 4; ++j) canonical[i * 20 + j] = static_cast<char>(bits >> (j * 8));
      for (unsigned j = 0; j < 8; ++j) {
        canonical[i * 20 + 4 + j] = static_cast<char>(sequences_[i] >> (j * 8));
        canonical[i * 20 + 12 + j] = static_cast<char>(times_[i] >> (j * 8));
      }
    }
    s.evidence_hash = telemetry_contract::sha256(std::string_view(canonical.data(), count_ * 20));
    s.ready = configured_ && rejected_ == 0 && count_ == criteria_.samples &&
      last_us_ - first_us_ >= criteria_.min_duration_us &&
      std::isfinite(s.variation_percent) && s.variation_percent <= criteria_.max_variation_percent;
    return s;
  }
 private:
  Mq135ReferenceCriteria criteria_{};
  std::array<float, 64> values_{};
  std::array<uint64_t, 64> sequences_{}, times_{};
  uint16_t count_{0}, rejected_{0};
  uint64_t sequence_{0}, first_us_{0}, last_us_{0};
  bool configured_{false};
};

struct Mq135GasProfile final {
  ProfileManifest manifest{};
  Mq135Electrical electrical{};
  config::ConfigHash adc_hash{};
  uint32_t adc_version{0};
  float r0_ohm{0};
  Mq135ReferenceSummary reference{};
  Mq135ReferenceCriteria criteria{};
  std::array<char, 32> hardware_identity{};
  std::array<char, 32> reference_environment{};
  int32_t temperature_centi_c{0};
  uint32_t humidity_centi_percent{0};
  uint64_t heating_duration_us{0};
  bool preparation_confirmed{false};
  bool valid() const noexcept {
    return profile_validity_allows_conversion(manifest.validity) &&
      manifest.kind == ProfileKind::Calibration && manifest.schema_version == 1 &&
      manifest.profile_version > 0 && manifest.target_id == 8 &&
      electrical.valid() && adc_version > 0 &&
      std::any_of(adc_hash.begin(), adc_hash.end(), [](uint8_t x) { return x != 0; }) &&
      criteria.valid() && reference.ready && reference.rejected == 0 &&
      reference.count == criteria.samples && reference.last_us >= reference.first_us &&
      reference.last_us - reference.first_us >= criteria.min_duration_us &&
      std::isfinite(r0_ohm) && r0_ohm > 0 && r0_ohm == reference.median_ohm &&
      std::isfinite(reference.minimum_ohm) && reference.minimum_ohm > 0 &&
      std::isfinite(reference.maximum_ohm) && reference.maximum_ohm >= reference.minimum_ohm &&
      r0_ohm >= reference.minimum_ohm && r0_ohm <= reference.maximum_ohm &&
      std::isfinite(reference.variation_percent) && reference.variation_percent >= 0 &&
      reference.variation_percent <= criteria.max_variation_percent &&
      std::any_of(reference.evidence_hash.begin(), reference.evidence_hash.end(), [](uint8_t x) { return x != 0; }) &&
      hardware_identity[0] && hardware_identity.back() == '\0' &&
      reference_environment[0] && reference_environment.back() == '\0' &&
      humidity_centi_percent <= 10000 && heating_duration_us > 0 && preparation_confirmed &&
      manifest.provenance.present && manifest.provenance.evidence_count == reference.count;
  }
};
inline constexpr ProfileKey kMq135GasKey{ProfileKind::Calibration, ProfileTarget::Mq135};

// Immutable control-result evidence. Replay caches this snapshot, never a new
// timestamp or a last-good operational gas reading. It is not telemetry-v3.
struct Mq135Diagnostic final {
  bool present{false}, source_present{false}, raw_present{false}, voltage_present{false};
  bool rs_present{false}, ratio_present{false}, profile_present{false}, circuit_present{false};
  bool clock_known{false}, age_known{false}, fresh{false};
  std::array<char, 32> quality{}, reason{}, hardware_identity{}, reference_environment{};
  std::array<char, 16> profile_state{};
  uint16_t raw_counts{0}, sample_count{0};
  float adc_pin_mv{0}, ao_mv{0}, rs_ohm{0}, r0_ohm{0}, ratio{0}, variation_percent{0};
  Mq135Electrical electrical{};
  ProfileRef profile{}, adc_profile{}, previous_profile{};
  BootId source_boot_id{};
  uint64_t source_sequence{0}, source_us{0}, response_us{0}, age_us{0}, max_age_us{0};
  int32_t temperature_centi_c{0};
  uint32_t humidity_centi_pct{0};
  RecordedAt calibration_recorded_at{};
  TimeQuality calibration_time_quality{TimeQuality::Unsynced};
};

inline bool mq135_ratio(float rs, float r0, float& ratio) noexcept {
  ratio = 0;
  if (!std::isfinite(rs) || !std::isfinite(r0) || rs <= 0 || r0 <= 0) return false;
  const double value = static_cast<double>(rs) / r0;
  if (!std::isfinite(value) || value <= 0 || !std::isfinite(static_cast<float>(value))) return false;
  ratio = static_cast<float>(value); return ratio > 0;
}

// RL-free normalized response of the same divider topology used by
// mq135_resistance(): with Vc the sensor supply, AO the reconstructed sensor
// output and RL the (possibly unknown) module load,
//   Rs = RL * (Vc / AO - 1), so q = Vc / AO - 1 and Rs/R0 = q / q0.
// The module load cancels, so an unknown RL must not block this arithmetic.
// A constant ADC voltage gain error does NOT cancel: it changes both AO samples
// and therefore survives into q/q0. These helpers are pure arithmetic on
// explicitly supplied voltages; they assert nothing about measurement
// provenance, ADC calibration, baseline validation, selectivity or accuracy.
// Unavailable input yields a zeroed output, never a zero/last-good ppm.
inline bool mq135_response(float circuit_mv, float ao_mv, float& q) noexcept {
  q = 0;
  if (!std::isfinite(circuit_mv) || circuit_mv <= 0) return false;
  if (!std::isfinite(ao_mv) || ao_mv <= 0 || ao_mv >= circuit_mv) return false;
  const double value = static_cast<double>(circuit_mv) / ao_mv - 1.0;
  // An overflowing quotient is not a response either.
  if (!std::isfinite(value) || value <= 0 ||
      value > static_cast<double>(std::numeric_limits<float>::max())) return false;
  q = static_cast<float>(value); return q > 0;
}

inline bool mq135_ratio_normalized(float q, float q0, float& ratio) noexcept {
  ratio = 0;
  if (!std::isfinite(q) || q <= 0 || !std::isfinite(q0) || q0 <= 0) return false;
  const double value = static_cast<double>(q) / q0;
  if (!std::isfinite(value) || value <= 0 || !std::isfinite(static_cast<float>(value))) return false;
  ratio = static_cast<float>(value); return ratio > 0;
}
} // namespace sigap::acquisition
