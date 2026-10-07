#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>

#include "models/model_profiles.h"

namespace sigap::safety {

inline constexpr std::size_t kSafetyTraceChannels{
    std::tuple_size<decltype(models::SafetyModelProfile{}.channels)>::value};

enum class HazardInputState : uint8_t {
  Ready,
  Missing,
  Stale,
  Calibrating,
  Uncalibrated,
  Timeout,
  Invalid,
};

struct HazardValue final {
  models::HazardSignal signal{models::HazardSignal::TemperatureC};
  HazardInputState state{HazardInputState::Missing};
  float value{0.0F};
};

struct HazardInputFrame final {
  std::array<HazardValue, kSafetyTraceChannels> values{};
  uint8_t count{0U};
};

enum class SafetyState : uint8_t {
  Unconfigured,
  Unavailable,
  Fault,
  Clear,
  Alert,
};

enum class SafetyReason : uint8_t {
  SafetyNotConfigured,
  InvalidModel,
  HazardInputMissing,
  HazardInputStale,
  HazardInputCalibrating,
  HazardInputUncalibrated,
  HazardInputTimeout,
  HazardInputInvalid,
  HazardThresholdExceeded,
  AlertLatched,
  AllValidatedChannelsClear,
};

struct SafetyChannelEvidence final {
  models::HazardSignal signal{models::HazardSignal::TemperatureC};
  HazardInputState input_state{HazardInputState::Missing};
  float value{0.0F};
  float alert_at_or_above{0.0F};
  float clear_below{0.0F};
  bool threshold_exceeded{false};
  bool clear_condition_met{false};
};

struct SafetyTrace final {
  acquisition::ProfileRef model_ref{};
  std::array<SafetyChannelEvidence, kSafetyTraceChannels> evidence{};
  uint8_t evidence_count{0U};
  uint8_t clear_streak{0U};
  bool alert_latched_before{false};
  bool alert_latched_after{false};
};

struct SafetyResult final {
  SafetyState state{SafetyState::Unconfigured};
  SafetyReason reason{SafetyReason::SafetyNotConfigured};
  models::ModelDeployment model_state{models::ModelDeployment::Unconfigured};
  SafetyTrace trace{};
};

class SafetyEvaluator final {
 public:
  SafetyEvaluator() noexcept = default;
  SafetyEvaluator(const SafetyEvaluator&) = delete;
  SafetyEvaluator& operator=(const SafetyEvaluator&) = delete;
  SafetyEvaluator(SafetyEvaluator&&) = delete;
  SafetyEvaluator& operator=(SafetyEvaluator&&) = delete;

  // This object is an instance-owned B3 safety state machine. The owner must
  // keep one evaluator for the lifetime of the runtime, rather than creating
  // a temporary evaluator for each processing cycle. reset() is reserved for
  // an explicit maintenance/reboot boundary.
  SafetyResult evaluate(const models::SafetyModelProfile* profile,
                        const HazardInputFrame& inputs) noexcept;

  bool alert_latched() const noexcept { return alert_latched_; }
  uint8_t clear_streak() const noexcept { return clear_streak_; }

  // Explicit maintenance/reset boundary. A FIS collection score is not an
  // input to this class and cannot release a latched B3 safety alert.
  void reset() noexcept;

 private:
  static const HazardValue* find_input(const HazardInputFrame& inputs,
                                       models::HazardSignal signal) noexcept;
  static bool same_profile(const acquisition::ProfileRef& left,
                           const acquisition::ProfileRef& right) noexcept;
  SafetyResult unavailable(const models::SafetyModelProfile& profile,
                           SafetyState state,
                           SafetyReason reason,
                           SafetyTrace trace) noexcept;

  acquisition::ProfileRef last_model_ref_{};
  bool alert_latched_{false};
  uint8_t clear_streak_{0U};
};

static_assert(kSafetyTraceChannels <= 8U,
              "Safety trace must remain bounded");

}  // namespace sigap::safety
