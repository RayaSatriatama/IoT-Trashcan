#include "safety/safety.h"

#include <cmath>
#include <cstddef>

namespace sigap::safety {
namespace {

SafetyReason reason_for_input_state(HazardInputState state) noexcept {
  switch (state) {
    case HazardInputState::Missing:
      return SafetyReason::HazardInputMissing;
    case HazardInputState::Stale:
      return SafetyReason::HazardInputStale;
    case HazardInputState::Calibrating:
      return SafetyReason::HazardInputCalibrating;
    case HazardInputState::Uncalibrated:
      return SafetyReason::HazardInputUncalibrated;
    case HazardInputState::Timeout:
      return SafetyReason::HazardInputTimeout;
    case HazardInputState::Invalid:
    case HazardInputState::Ready:
      return SafetyReason::HazardInputInvalid;
  }
  return SafetyReason::HazardInputInvalid;
}

SafetyState state_for_input_state(HazardInputState state) noexcept {
  return state == HazardInputState::Invalid ? SafetyState::Fault
                                            : SafetyState::Unavailable;
}

}  // namespace

const HazardValue* SafetyEvaluator::find_input(
    const HazardInputFrame& inputs, models::HazardSignal signal) noexcept {
  const auto count = inputs.count <= inputs.values.size()
                         ? inputs.count
                         : static_cast<uint8_t>(inputs.values.size());
  for (uint8_t index = 0U; index < count; ++index) {
    if (inputs.values[index].signal == signal) {
      return &inputs.values[index];
    }
  }
  return nullptr;
}

bool SafetyEvaluator::same_profile(
    const acquisition::ProfileRef& left,
    const acquisition::ProfileRef& right) noexcept {
  return left.present == right.present && left.kind == right.kind &&
         left.schema_version == right.schema_version &&
         left.profile_version == right.profile_version &&
         left.target_id == right.target_id && left.hash == right.hash;
}

SafetyResult SafetyEvaluator::unavailable(
    const models::SafetyModelProfile& profile,
    SafetyState state,
    SafetyReason reason,
    SafetyTrace trace) noexcept {
  clear_streak_ = 0U;
  trace.clear_streak = clear_streak_;
  trace.alert_latched_after = alert_latched_;
  return SafetyResult{state, reason, profile.deployment, trace};
}

SafetyResult SafetyEvaluator::evaluate(
    const models::SafetyModelProfile* profile,
    const HazardInputFrame& inputs) noexcept {
  if (profile == nullptr) {
    clear_streak_ = 0U;
    return SafetyResult{};
  }

  SafetyTrace trace{};
  trace.model_ref = profile->ref();
  trace.alert_latched_before = alert_latched_;

  if (!models::validate_safety_model(*profile).valid()) {
    return unavailable(*profile, SafetyState::Unavailable,
                       SafetyReason::InvalidModel, trace);
  }

  if (!same_profile(last_model_ref_, trace.model_ref)) {
    // A model activation does not clear an existing alert. It only restarts
    // the new model's validated consecutive-clear observation window.
    clear_streak_ = 0U;
    last_model_ref_ = trace.model_ref;
  }

  if (inputs.count > inputs.values.size()) {
    return unavailable(*profile, SafetyState::Fault,
                       SafetyReason::HazardInputInvalid, trace);
  }

  bool threshold_exceeded = false;
  bool all_clear_conditions_met = true;

  for (uint8_t rule_index = 0U; rule_index < profile->channel_count;
       ++rule_index) {
    const auto& rule = profile->channels[rule_index];
    const auto* input = find_input(inputs, rule.signal);
    if (input == nullptr) {
      if (rule.required) {
        return unavailable(*profile, SafetyState::Unavailable,
                           SafetyReason::HazardInputMissing, trace);
      }
      continue;
    }

    for (uint8_t duplicate_index = 0U; duplicate_index < inputs.count;
         ++duplicate_index) {
      if (&inputs.values[duplicate_index] != input &&
          inputs.values[duplicate_index].signal == rule.signal) {
        return unavailable(*profile, SafetyState::Fault,
                           SafetyReason::HazardInputInvalid, trace);
      }
    }

    if (input->state != HazardInputState::Ready) {
      return unavailable(*profile, state_for_input_state(input->state),
                         reason_for_input_state(input->state), trace);
    }
    if (!std::isfinite(input->value)) {
      return unavailable(*profile, SafetyState::Fault,
                         SafetyReason::HazardInputInvalid, trace);
    }

    auto& evidence = trace.evidence[trace.evidence_count++];
    evidence.signal = rule.signal;
    evidence.input_state = input->state;
    evidence.value = input->value;
    evidence.alert_at_or_above = rule.alert_at_or_above;
    evidence.clear_below = rule.clear_below;
    evidence.threshold_exceeded = input->value >= rule.alert_at_or_above;
    evidence.clear_condition_met = input->value <= rule.clear_below;
    threshold_exceeded = threshold_exceeded || evidence.threshold_exceeded;
    all_clear_conditions_met =
        all_clear_conditions_met && evidence.clear_condition_met;
  }

  if (trace.evidence_count == 0U) {
    return unavailable(*profile, SafetyState::Unavailable,
                       SafetyReason::HazardInputMissing, trace);
  }

  if (threshold_exceeded) {
    alert_latched_ = true;
    clear_streak_ = 0U;
    trace.clear_streak = clear_streak_;
    trace.alert_latched_after = true;
    return SafetyResult{SafetyState::Alert,
                        SafetyReason::HazardThresholdExceeded,
                        profile->deployment,
                        trace};
  }

  if (alert_latched_) {
    if (all_clear_conditions_met) {
      if (clear_streak_ < profile->clear_consecutive_cycles) {
        ++clear_streak_;
      }
      if (clear_streak_ >= profile->clear_consecutive_cycles) {
        alert_latched_ = false;
        trace.clear_streak = clear_streak_;
        trace.alert_latched_after = false;
        return SafetyResult{SafetyState::Clear,
                            SafetyReason::AllValidatedChannelsClear,
                            profile->deployment,
                            trace};
      }
    } else {
      clear_streak_ = 0U;
    }

    trace.clear_streak = clear_streak_;
    trace.alert_latched_after = true;
    return SafetyResult{SafetyState::Alert,
                        SafetyReason::AlertLatched,
                        profile->deployment,
                        trace};
  }

  clear_streak_ = 0U;
  trace.clear_streak = clear_streak_;
  trace.alert_latched_after = false;
  return SafetyResult{SafetyState::Clear,
                      SafetyReason::AllValidatedChannelsClear,
                      profile->deployment,
                      trace};
}

void SafetyEvaluator::reset() noexcept {
  last_model_ref_ = acquisition::ProfileRef{};
  alert_latched_ = false;
  clear_streak_ = 0U;
}

}  // namespace sigap::safety
