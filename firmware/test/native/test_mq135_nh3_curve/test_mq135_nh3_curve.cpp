// S1 focused regression: official-Winsen digitized NH3 curve table and the
// bounded piecewise log-log research estimator.
// The curve is a digitized typical Winsen v1.6 Figure 3 curve. It is a research
// approximation with per-point digitization uncertainty, NOT a manufacturer
// numeric specification, NOT a selective NH3 measurement and NOT a
// laboratory-calibrated value. No ppm below is a physical measurement.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <unity.h>

// Unity in this project is built without UNITY_INCLUDE_DOUBLE, so the double
// assertions are expressed explicitly. Real firmware numeric type is double.
#define ASSERT_DOUBLE_WITHIN(tolerance, expected, actual)                     \
  TEST_ASSERT_TRUE_MESSAGE(std::fabs((expected) - (actual)) <= (tolerance),   \
                           #actual " differs from " #expected)
#define ASSERT_DOUBLE_EXACTLY(expected, actual)                               \
  TEST_ASSERT_TRUE_MESSAGE((expected) == (actual), #actual " != " #expected)

#include "acquisition/mq135_nh3.h"
#include "telemetry/sha256.h"

using namespace sigap::acquisition;

void setUp() {}
void tearDown() {}

namespace {

std::string read_csv() {
  const char* candidates[] = {"docs/mq135-winsen-nh3-curve.csv",
                              "../docs/mq135-winsen-nh3-curve.csv",
                              "../../docs/mq135-winsen-nh3-curve.csv"};
  for (const char* path : candidates) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) continue;
    std::string bytes;
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, read);
    std::fclose(file);
    return bytes;
  }
  return {};
}

struct CsvRow {
  double ppm;
  double ratio;
  double sigma;
};

std::vector<CsvRow> parse_rows(const std::string& text) {
  std::vector<CsvRow> rows;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line =
        text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line[0] != '#') {
      // concentration_ppm,fig3_col_px,row_px,rs_r0,sigma_pct,contaminated,note
      const char* cursor = line.c_str();
      char* next = nullptr;
      const double ppm = std::strtod(cursor, &next);
      if (next != cursor) {
        for (int field = 0; field < 2; ++field) {
          cursor = next + 1;
          std::strtod(cursor, &next);
        }
        cursor = next + 1;
        const double ratio = std::strtod(cursor, &next);
        cursor = next + 1;
        const double sigma = std::strtod(cursor, &next);
        if (ratio > 0.0 && sigma > 0.0) rows.push_back(CsvRow{ppm, ratio, sigma});
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return rows;
}

constexpr double kGeometricMidpointPpm[12] = {
    12.247448713915889, 17.320508075688770, 24.494897427831780,
    38.729833462074170, 59.160797830996150, 83.666002653407550,
    122.474487139158920, 173.205080756887750, 244.948974278317650,
    387.298334620741570, 591.607978309961700, 836.660026534075700};

}  // namespace

// The compiled table must be bound to the reviewed source bytes.
void test_curve_table_is_source_digest_bound_and_matches_the_csv() {
  const std::string csv = read_csv();
  TEST_ASSERT_TRUE_MESSAGE(!csv.empty(), "docs/mq135-winsen-nh3-curve.csv must exist");

  const auto digest = sigap::telemetry_contract::sha256(csv);
  TEST_ASSERT_EQUAL_MEMORY(kMq135Nh3CurveSourceSha256.data(), digest.data(), digest.size());
  TEST_ASSERT_EQUAL_MEMORY(kMq135Nh3Curve.source_sha256.data(), digest.data(), digest.size());

  const std::vector<CsvRow> rows = parse_rows(csv);
  TEST_ASSERT_EQUAL_UINT32(13U, rows.size());
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMq135Nh3Curve.count), rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    ASSERT_DOUBLE_EXACTLY(rows[index].ppm, kMq135Nh3Curve.points[index].concentration_ppm);
    ASSERT_DOUBLE_EXACTLY(rows[index].ratio, kMq135Nh3Curve.points[index].rs_r0);
    ASSERT_DOUBLE_EXACTLY(rows[index].sigma, kMq135Nh3Curve.points[index].sigma_percent);
  }
  TEST_ASSERT_EQUAL_UINT16(2U, kMq135Nh3Curve.version);
  ASSERT_DOUBLE_EXACTLY(10.0, kMq135Nh3Curve.points[0].concentration_ppm);
  ASSERT_DOUBLE_EXACTLY(0.6559, kMq135Nh3Curve.points[0].rs_r0);
  ASSERT_DOUBLE_EXACTLY(1000.0, kMq135Nh3Curve.points[12].concentration_ppm);
  ASSERT_DOUBLE_EXACTLY(0.0893, kMq135Nh3Curve.points[12].rs_r0);
  TEST_ASSERT_TRUE(mq135_nh3_curve_valid(kMq135Nh3Curve));
}

void test_curve_structure_is_monotonic_and_bounded() {
  const auto& curve = kMq135Nh3Curve;
  TEST_ASSERT_TRUE(curve.available());
  TEST_ASSERT_TRUE(mq135_nh3_curve_valid(curve));
  for (std::size_t index = 0; index < curve.count; ++index) {
    TEST_ASSERT_TRUE(curve.points[index].concentration_ppm > 0.0);
    TEST_ASSERT_TRUE(curve.points[index].rs_r0 > 0.0);
    TEST_ASSERT_TRUE(curve.points[index].sigma_percent > 0.0);
    TEST_ASSERT_TRUE(curve.points[index].sigma_percent <= 5.0);
    if (index > 0) {
      // Resistance ratio decreases with increasing NH3 concentration.
      TEST_ASSERT_TRUE(curve.points[index].concentration_ppm >
                       curve.points[index - 1].concentration_ppm);
      TEST_ASSERT_TRUE(curve.points[index].rs_r0 < curve.points[index - 1].rs_r0);
    }
  }
  TEST_ASSERT_FALSE(curve.production_eligible());
}

// Every reviewed source point must be recovered exactly, in the real double type.
void test_exact_source_point_recovery() {
  for (std::size_t index = 0; index < kMq135Nh3Curve.count; ++index) {
    const auto& point = kMq135Nh3Curve.points[index];
    const Mq135Nh3CurveEstimate estimate = mq135_nh3_curve_estimate(kMq135Nh3Curve, point.rs_r0);
    TEST_ASSERT_TRUE(estimate.available());
    ASSERT_DOUBLE_EXACTLY(point.concentration_ppm, estimate.ppm);
    ASSERT_DOUBLE_EXACTLY(point.rs_r0, estimate.ratio);
    TEST_ASSERT_EQUAL_UINT16(kMq135Nh3CurveId, estimate.curve_id);
    TEST_ASSERT_EQUAL_UINT16(kMq135Nh3CurveVersion, estimate.curve_version);
  }
}

void test_every_segment_interpolates_piecewise_log_log() {
  for (std::size_t index = 0; index + 1 < kMq135Nh3Curve.count; ++index) {
    const auto& low = kMq135Nh3Curve.points[index];
    const auto& high = kMq135Nh3Curve.points[index + 1];
    const double ratio = std::sqrt(low.rs_r0 * high.rs_r0);
    const Mq135Nh3CurveEstimate estimate = mq135_nh3_curve_estimate(kMq135Nh3Curve, ratio);
    TEST_ASSERT_TRUE(estimate.available());
    // Independent fixture: geometric-mean ratio maps to the geometric-mean ppm.
    const double expected = std::sqrt(low.concentration_ppm * high.concentration_ppm);
    ASSERT_DOUBLE_WITHIN(expected * 1e-12, expected, estimate.ppm);
    ASSERT_DOUBLE_WITHIN(1e-12, expected, kGeometricMidpointPpm[index]);
    TEST_ASSERT_TRUE(estimate.ppm > low.concentration_ppm);
    TEST_ASSERT_TRUE(estimate.ppm < high.concentration_ppm);
  }
}

// Lower ratio must always mean higher estimated concentration, and vice versa.
void test_estimate_direction_is_monotonic_in_ratio() {
  double previous_ppm = 0.0;
  for (int step = 0; step <= 200; ++step) {
    const double ratio =
        step == 0 ? kMq135Nh3CurveMaxRatio
                  : (step == 200
                         ? kMq135Nh3CurveMinRatio
                         : kMq135Nh3CurveMaxRatio -
                               (kMq135Nh3CurveMaxRatio - kMq135Nh3CurveMinRatio) *
                                   static_cast<double>(step) / 200.0);
    const Mq135Nh3CurveEstimate estimate = mq135_nh3_curve_estimate(kMq135Nh3Curve, ratio);
    TEST_ASSERT_TRUE(estimate.available());
    TEST_ASSERT_TRUE(estimate.ppm >= previous_ppm);
    previous_ppm = estimate.ppm;
  }
  ASSERT_DOUBLE_EXACTLY(10.0,
                           mq135_nh3_curve_estimate(kMq135Nh3Curve, kMq135Nh3CurveMaxRatio).ppm);
  ASSERT_DOUBLE_EXACTLY(1000.0, previous_ppm);
}

// Exact domain checks: 0.6559 -> 10 and 0.0893 -> 1000, no clamp, no extrapolation.
void test_domain_endpoints_are_exact_and_outside_domain_is_never_clamped() {
  const Mq135Nh3CurveEstimate low = mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.6559);
  TEST_ASSERT_TRUE(low.available());
  ASSERT_DOUBLE_EXACTLY(10.0, low.ppm);

  const Mq135Nh3CurveEstimate high = mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.0893);
  TEST_ASSERT_TRUE(high.available());
  ASSERT_DOUBLE_EXACTLY(1000.0, high.ppm);

  // 0.0902 is a valid interior ratio, not a clamped 1000 ppm endpoint.
  const Mq135Nh3CurveEstimate interior = mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.0902);
  TEST_ASSERT_TRUE(interior.available());
  ASSERT_DOUBLE_WITHIN(1e-6, 978.0301440862712, interior.ppm);
  TEST_ASSERT_TRUE(interior.ppm < 1000.0);

  const Mq135Nh3CurveEstimate above = mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.6576);
  TEST_ASSERT_FALSE(above.available());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::RatioAboveDomain),
                        static_cast<int>(above.status));
  ASSERT_DOUBLE_EXACTLY(0.0, above.ppm);

  TEST_ASSERT_FALSE(mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.7).available());
  const Mq135Nh3CurveEstimate below = mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.08);
  TEST_ASSERT_FALSE(below.available());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::RatioBelowDomain),
                        static_cast<int>(below.status));
  ASSERT_DOUBLE_EXACTLY(0.0, below.ppm);
}

void test_nonfinite_and_nonpositive_ratio_is_invalid() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::InvalidRatio),
                        static_cast<int>(
                            mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.0).status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::InvalidRatio),
                        static_cast<int>(
                            mq135_nh3_curve_estimate(kMq135Nh3Curve, -0.3).status));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(Mq135Nh3CurveStatus::InvalidRatio),
      static_cast<int>(mq135_nh3_curve_estimate(
                           kMq135Nh3Curve, std::numeric_limits<double>::quiet_NaN())
                           .status));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(Mq135Nh3CurveStatus::InvalidRatio),
      static_cast<int>(
          mq135_nh3_curve_estimate(kMq135Nh3Curve, std::numeric_limits<double>::infinity())
              .status));
  const Mq135Nh3CurveEstimate estimate = mq135_nh3_curve_estimate(kMq135Nh3Curve, -1.0);
  TEST_ASSERT_FALSE(estimate.available());
  ASSERT_DOUBLE_EXACTLY(0.0, estimate.ppm);
}

// Unavailable, duplicated, reordered or non-finite tables must never estimate.
void test_unavailable_and_invalid_curve_tables_are_rejected() {
  const Mq135Nh3CurveEstimate unavailable = mq135_nh3_curve_estimate(Mq135Nh3Curve{}, 0.3);
  TEST_ASSERT_FALSE(unavailable.available());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveUnavailable),
                        static_cast<int>(unavailable.status));

  Mq135Nh3CurvePoint points[13];
  std::memcpy(points, kMq135Nh3Curve.points, sizeof(points));

  Mq135Nh3Curve broken = kMq135Nh3Curve;
  broken.points = nullptr;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveUnavailable),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  broken = kMq135Nh3Curve;
  broken.count = 1;
  // Fewer than two points cannot bracket any ratio.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveUnavailable),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  broken = kMq135Nh3Curve;
  broken.points = points;
  points[4].concentration_ppm = points[3].concentration_ppm;  // duplicate ppm
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  std::memcpy(points, kMq135Nh3Curve.points, sizeof(points));
  points[4].rs_r0 = points[3].rs_r0;  // duplicate ratio
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  std::memcpy(points, kMq135Nh3Curve.points, sizeof(points));
  const Mq135Nh3CurvePoint swap = points[2];  // reordered concentrations
  points[2] = points[3];
  points[3] = swap;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  std::memcpy(points, kMq135Nh3Curve.points, sizeof(points));
  points[6].rs_r0 = std::numeric_limits<double>::quiet_NaN();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  std::memcpy(points, kMq135Nh3Curve.points, sizeof(points));
  points[0].rs_r0 = 0.0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  std::memcpy(points, kMq135Nh3Curve.points, sizeof(points));
  points[9].concentration_ppm = -300.0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(broken, 0.3).status));

  std::memcpy(points, kMq135Nh3Curve.points, sizeof(points));
  Mq135Nh3Curve unbound = kMq135Nh3Curve;
  unbound.source_sha256.fill(0);  // not bound to a reviewed source
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(unbound, 0.3).status));

  unbound = kMq135Nh3Curve;
  unbound.version = 0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::CurveInvalid),
                        static_cast<int>(mq135_nh3_curve_estimate(unbound, 0.3).status));
}

// The reviewed domain is exact in double. A float-rounded top endpoint widens
// above the reviewed maximum and must be rejected, never clamped to 10 ppm.
void test_float_representation_at_the_domain_edge_is_not_clamped() {
  const float narrowed = static_cast<float>(kMq135Nh3CurveMaxRatio);
  const double widened = static_cast<double>(narrowed);
  TEST_ASSERT_TRUE(widened > kMq135Nh3CurveMaxRatio);
  const Mq135Nh3CurveEstimate rejected = mq135_nh3_curve_estimate(kMq135Nh3Curve, widened);
  TEST_ASSERT_FALSE(rejected.available());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Mq135Nh3CurveStatus::RatioAboveDomain),
                        static_cast<int>(rejected.status));

  // A float ratio inside the domain still estimates.
  const Mq135Nh3CurveEstimate inside = mq135_nh3_curve_estimate(kMq135Nh3Curve, 0.35);
  TEST_ASSERT_TRUE(inside.available());
  ASSERT_DOUBLE_WITHIN(0.001, 52.681941580194255, inside.ppm);
}

// The pre-existing synthetic log-regression research helper is preserved.
void test_existing_synthetic_research_helper_is_preserved() {
  const Mq135Nh3Point points[]{
      {1, 2, 25, 50, true}, {2, 1, 25, 50, true}, {4, .5, 25, 50, true}, {8, .25, 25, 50, true}};
  Mq135Nh3Model model{};
  TEST_ASSERT_TRUE(fit_mq135_nh3_test_model(points, 4, "SYNTHETIC", 1, model));
  TEST_ASSERT_TRUE(std::fabs(model.curve_a - 2.0) < 1e-9);
  TEST_ASSERT_TRUE(std::fabs(model.curve_b + 1.0) < 1e-9);
  double ppm{};
  TEST_ASSERT_TRUE(mq135_nh3_test_estimate(model, .5, ppm));
  TEST_ASSERT_TRUE(std::fabs(ppm - 4.0) < 1e-9);
  TEST_ASSERT_FALSE(model.production_eligible());
  TEST_ASSERT_FALSE(mq135_nh3_test_estimate(model, .01, ppm));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_curve_table_is_source_digest_bound_and_matches_the_csv);
  RUN_TEST(test_curve_structure_is_monotonic_and_bounded);
  RUN_TEST(test_exact_source_point_recovery);
  RUN_TEST(test_every_segment_interpolates_piecewise_log_log);
  RUN_TEST(test_estimate_direction_is_monotonic_in_ratio);
  RUN_TEST(test_domain_endpoints_are_exact_and_outside_domain_is_never_clamped);
  RUN_TEST(test_nonfinite_and_nonpositive_ratio_is_invalid);
  RUN_TEST(test_unavailable_and_invalid_curve_tables_are_rejected);
  RUN_TEST(test_float_representation_at_the_domain_edge_is_not_clamped);
  RUN_TEST(test_existing_synthetic_research_helper_is_preserved);
  return UNITY_END();
}
