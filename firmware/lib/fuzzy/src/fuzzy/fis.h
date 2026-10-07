#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "models/model_profiles.h"

namespace sigap::fuzzy {

inline constexpr std::size_t kMaxTraceMemberships{16U};
inline constexpr std::size_t kMaxTraceRules{16U};

enum class FisInputState : uint8_t {
  Missing,
  Ready,
  Stale,
  Calibrating,
  Uncalibrated,
  Invalid,
};

struct FisInput final {
  FisInputState state{FisInputState::Missing};
  float value{0.0F};
};

struct FisInputs final {
  std::size_t count{0U};
  std::array<FisInput, models::kMaxFisInputs> values{};
};

enum class FisState : uint8_t {
  Unconfigured,
  Unavailable,
  Ready,
};

enum class FisReason : uint8_t {
  ModelNotConfigured,
  InvalidModel,
  TraceOverflow,
  InputMissing,
  InputStale,
  InputCalibrating,
  InputUncalibrated,
  InputInvalid,
  InputOutOfDomain,
  ZeroArea,
  Ready,
};

struct FisModelRef final {
  uint32_t version{0U};
  std::array<uint8_t, 32U> hash{};
  bool present{false};
};

struct MembershipTrace final {
  models::ModelInput input{models::ModelInput::FillPercent};
  models::ModelId set_id{};
  float value{0.0F};
};

struct RuleTrace final {
  uint16_t id{0U};
  float strength{0.0F};
  models::ModelId output_id{};
};

struct AggregateTrace final {
  bool present{false};
  models::ModelId output_id{};
  float area{0.0F};
  bool centroid_present{false};
  float centroid{0.0F};
};

struct FisTrace final {
  FisModelRef model_ref{};
  std::size_t membership_count{0U};
  std::array<MembershipTrace, kMaxTraceMemberships> memberships{};
  std::size_t rule_count{0U};
  std::array<RuleTrace, kMaxTraceRules> rules{};
  AggregateTrace aggregate{};
};

struct FisResult final {
  FisState state{FisState::Unavailable};
  FisReason reason{FisReason::InvalidModel};
  models::ModelDeployment model_state{models::ModelDeployment::Unconfigured};
  bool score_present{false};
  float score{0.0F};
  bool label_present{false};
  models::ModelId label{};
  bool trace_present{false};
  FisTrace trace{};
};

float evaluate_membership(const models::MembershipFunction& function,
                          float input) noexcept;

class MamdaniEvaluator final {
 public:
  static FisResult evaluate(const models::FisModelProfile& profile,
                            const FisInputs& inputs) noexcept;
};

}  // namespace sigap::fuzzy
