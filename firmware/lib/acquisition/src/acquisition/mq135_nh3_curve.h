#pragma once

// Official-Winsen digitized NH3 sensitivity curve (MQ-135, Figure 3 of the
// Winsen v1.6 manual) and a bounded piecewise log-log research estimator.
//
// Provenance: docs/mq135-winsen-nh3-curve.csv, SHA256
// 4aa984238efa7d253b77ac828babd40e3a8f10ef14a3a86f8b9f1da0e6dd414b, column
// `rs_r0`, curve_version 2, 13 reviewed source points, domain ratio
// 0.0893..0.6559 and nominal concentration 10..1000 ppm.
//
// This table is a DIGITIZED TYPICAL curve, not manufacturer-specified numeric
// data, not a selective NH3 measurement, not a certified calibration and not a
// validated physical accuracy claim. sigma_percent is the reviewed per-point
// DIGITIZATION uncertainty only; it is not a physical concentration
// uncertainty and no Figure 4 H2 compensation is applied.
//
// The estimator never extrapolates, never clamps to the domain endpoints and
// never returns a zero/last-good ppm. It is not connected to any active
// profile, persistence, registration or output path; production_eligible()
// stays false.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sigap::acquisition {

// Curve identity as persisted/reported by later slices. 0 means "none".
inline constexpr uint16_t kMq135Nh3CurveId{0x4E48U};  // 'N','H' research table
inline constexpr uint16_t kMq135Nh3CurveVersion{2U};

// Exact reviewed source domain. Ratios outside these bounds are out of range.
inline constexpr double kMq135Nh3CurveMinRatio{0.0893};
inline constexpr double kMq135Nh3CurveMaxRatio{0.6559};
inline constexpr double kMq135Nh3CurveMinPpm{10.0};
inline constexpr double kMq135Nh3CurveMaxPpm{1000.0};

// Bounded capacity of a curve table; the reviewed table uses 13 of these.
inline constexpr std::size_t kMq135Nh3CurveCapacity{16U};

// SHA256 of the exact accepted CSV bytes. Every table must carry this digest;
// a table that does not is treated as unbound and never estimates.
inline constexpr std::array<uint8_t, 32> kMq135Nh3CurveSourceSha256{
    0x4aU, 0xa9U, 0x84U, 0x23U, 0x8eU, 0xfaU, 0x7dU, 0x25U,
    0x3bU, 0x77U, 0xacU, 0x82U, 0x8bU, 0xabU, 0xd4U, 0x0eU,
    0x3aU, 0x8fU, 0x10U, 0xefU, 0x14U, 0xa3U, 0xa8U, 0x6fU,
    0x8bU, 0x9fU, 0x1dU, 0xa0U, 0xe6U, 0xddU, 0x41U, 0x4bU};

struct Mq135Nh3CurvePoint final {
  double concentration_ppm{};
  double rs_r0{};
  double sigma_percent{};
};

struct Mq135Nh3Curve final {
  uint16_t curve_id{0U};
  uint16_t version{0U};
  std::array<uint8_t, 32> source_sha256{};
  const Mq135Nh3CurvePoint* points{nullptr};
  std::size_t count{0U};

  constexpr bool available() const noexcept {
    return points != nullptr && count >= 2U;
  }
  // Research evidence only: no registration, persistence or ppm output.
  constexpr bool production_eligible() const noexcept { return false; }
};

// Reviewed values, byte-for-byte equivalent to docs/mq135-winsen-nh3-curve.csv.
inline constexpr std::array<Mq135Nh3CurvePoint, 13> kMq135Nh3CurveSourcePoints{{
    {10.0, 0.6559, 2.00},
    {15.0, 0.5814, 0.70},
    {20.0, 0.5180, 0.70},
    {30.0, 0.4395, 0.74},
    {50.0, 0.3576, 0.70},
    {70.0, 0.3114, 0.70},
    {100.0, 0.2668, 1.18},
    {150.0, 0.2202, 0.71},
    {200.0, 0.1872, 1.05},
    {300.0, 0.1542, 0.70},
    {500.0, 0.1210, 0.70},
    {700.0, 0.1049, 0.70},
    {1000.0, 0.0893, 1.38},
}};

inline constexpr Mq135Nh3Curve kMq135Nh3Curve{
    kMq135Nh3CurveId,
    kMq135Nh3CurveVersion,
    kMq135Nh3CurveSourceSha256,
    kMq135Nh3CurveSourcePoints.data(),
    kMq135Nh3CurveSourcePoints.size()};

enum class Mq135Nh3CurveStatus : uint8_t {
  None = 0,
  CurveUnavailable,
  CurveInvalid,
  InvalidRatio,
  RatioAboveDomain,  // ratio > 0.6559, i.e. below 10 ppm; no extrapolation
  RatioBelowDomain,  // ratio < 0.0893, i.e. above 1000 ppm; no extrapolation
};

struct Mq135Nh3CurveEstimate final {
  Mq135Nh3CurveStatus status{Mq135Nh3CurveStatus::CurveUnavailable};
  double ppm{};
  double ratio{};
  uint16_t curve_id{0U};
  uint16_t curve_version{0U};

  constexpr bool available() const noexcept { return status == Mq135Nh3CurveStatus::None; }
  // Historical digitization uncertainty of the two bracketing points, not a
  // physical concentration uncertainty. 0 when the estimate is unavailable.
  double bracket_sigma_percent{};

  // Concentration estimates remain research-only evidence.
  constexpr bool production_eligible() const noexcept { return false; }
};

// Structural validity: bound to the reviewed source digest, finite positive
// points, strictly increasing concentration, strictly decreasing ratio and
// exact reviewed endpoints. Duplicated, reordered, unbound or unusable tables
// are rejected rather than repaired.
inline bool mq135_nh3_curve_valid(const Mq135Nh3Curve& curve) noexcept {
  if (curve.points == nullptr || curve.count < 2U ||
      curve.count > kMq135Nh3CurveCapacity) {
    return false;
  }
  if (curve.curve_id != kMq135Nh3CurveId || curve.version != kMq135Nh3CurveVersion) {
    return false;
  }
  for (std::size_t index = 0; index < curve.source_sha256.size(); ++index) {
    if (curve.source_sha256[index] != kMq135Nh3CurveSourceSha256[index]) return false;
  }
  for (std::size_t index = 0; index < curve.count; ++index) {
    const Mq135Nh3CurvePoint& point = curve.points[index];
    if (!std::isfinite(point.concentration_ppm) || point.concentration_ppm <= 0.0) {
      return false;
    }
    if (!std::isfinite(point.rs_r0) || point.rs_r0 <= 0.0) return false;
    // Digitization uncertainty must be present and plausibly bounded.
    if (!std::isfinite(point.sigma_percent) || point.sigma_percent <= 0.0 ||
        point.sigma_percent > 5.0) {
      return false;
    }
    if (index > 0U) {
      if (!(point.concentration_ppm > curve.points[index - 1U].concentration_ppm)) {
        return false;
      }
      if (!(point.rs_r0 < curve.points[index - 1U].rs_r0)) return false;
    }
  }
  // The table must cover exactly the reviewed domain, not a wider one.
  return curve.points[0].rs_r0 == kMq135Nh3CurveMaxRatio &&
         curve.points[0].concentration_ppm == kMq135Nh3CurveMinPpm &&
         curve.points[curve.count - 1U].rs_r0 == kMq135Nh3CurveMinRatio &&
         curve.points[curve.count - 1U].concentration_ppm == kMq135Nh3CurveMaxPpm;
}

// Bounded piecewise log-log interpolation inside the reviewed ratio domain.
// Source points are reproduced exactly (no exp(log()) round-trip at a knot) and
// outside the domain the estimate is unavailable with an explicit status.
inline Mq135Nh3CurveEstimate mq135_nh3_curve_estimate(const Mq135Nh3Curve& curve,
                                                      double ratio) noexcept {
  Mq135Nh3CurveEstimate estimate{};
  if (!curve.available()) {
    estimate.status = Mq135Nh3CurveStatus::CurveUnavailable;
    return estimate;
  }
  estimate.curve_id = curve.curve_id;
  estimate.curve_version = curve.version;
  if (!std::isfinite(ratio) || ratio <= 0.0) {
    estimate.status = Mq135Nh3CurveStatus::InvalidRatio;
    return estimate;
  }
  if (!mq135_nh3_curve_valid(curve)) {
    estimate.status = Mq135Nh3CurveStatus::CurveInvalid;
    return estimate;
  }
  estimate.ratio = ratio;
  if (ratio > kMq135Nh3CurveMaxRatio) {
    estimate.status = Mq135Nh3CurveStatus::RatioAboveDomain;
    return estimate;
  }
  if (ratio < kMq135Nh3CurveMinRatio) {
    estimate.status = Mq135Nh3CurveStatus::RatioBelowDomain;
    return estimate;
  }
  for (std::size_t index = 0; index + 1U < curve.count; ++index) {
    const Mq135Nh3CurvePoint& low_ppm_point = curve.points[index];
    const Mq135Nh3CurvePoint& high_ppm_point = curve.points[index + 1U];
    if (ratio > low_ppm_point.rs_r0 || ratio < high_ppm_point.rs_r0) continue;
    estimate.bracket_sigma_percent =
        low_ppm_point.sigma_percent > high_ppm_point.sigma_percent
            ? low_ppm_point.sigma_percent
            : high_ppm_point.sigma_percent;
    // Exact knot recovery: never re-derive a source point through exp(log()).
    if (ratio == low_ppm_point.rs_r0) {
      estimate.ppm = low_ppm_point.concentration_ppm;
      estimate.status = Mq135Nh3CurveStatus::None;
      return estimate;
    }
    if (ratio == high_ppm_point.rs_r0) {
      estimate.ppm = high_ppm_point.concentration_ppm;
      estimate.status = Mq135Nh3CurveStatus::None;
      return estimate;
    }
    const double log_ratio = std::log(ratio);
    const double log_low_ratio = std::log(low_ppm_point.rs_r0);
    const double log_high_ratio = std::log(high_ppm_point.rs_r0);
    const double span = log_high_ratio - log_low_ratio;
    if (!std::isfinite(span) || span == 0.0) {
      estimate.status = Mq135Nh3CurveStatus::CurveInvalid;
      return estimate;
    }
    const double t = (log_ratio - log_low_ratio) / span;
    const double log_ppm = std::log(low_ppm_point.concentration_ppm) +
                           t * (std::log(high_ppm_point.concentration_ppm) -
                                std::log(low_ppm_point.concentration_ppm));
    const double ppm = std::exp(log_ppm);
    // Belt and braces: an interior result must stay inside the bracketing pair.
    if (!std::isfinite(ppm) || ppm < low_ppm_point.concentration_ppm ||
        ppm > high_ppm_point.concentration_ppm) {
      estimate.ppm = 0.0;
      estimate.status = Mq135Nh3CurveStatus::CurveInvalid;
      return estimate;
    }
    estimate.ppm = ppm;
    estimate.status = Mq135Nh3CurveStatus::None;
    return estimate;
  }
  // Unreachable for a valid table; never fabricate a value.
  estimate.ppm = 0.0;
  estimate.status = Mq135Nh3CurveStatus::CurveInvalid;
  return estimate;
}

}  // namespace sigap::acquisition
