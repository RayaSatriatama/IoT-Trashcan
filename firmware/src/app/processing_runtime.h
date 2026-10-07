#pragma once

#include <cstddef>

#include "fuzzy/fis.h"
#include "processing/processing_engine.h"
#include "safety/safety.h"

namespace sigap {

// Application-owned bounded handoff for Task 5 results. Model profiles are
// supplied per cycle by the future model/profile owner; no synthetic model is
// installed here when the device has no approved profile.
class ProcessingRuntime final {
 public:
  using Result = processing::EvaluatedResult<
      fuzzy::FisResult, safety::SafetyResult, 16U>;
  using Handoff = processing::EvaluatedResultRing<Result, 1U>;

  bool begin(const acquisition::ConfigRef& config_ref) noexcept {
    if (!config_ref.present || config_ref.schema_version == 0U) {
      enabled_ = false;
      config_ref_ = acquisition::ConfigRef{};
      return false;
    }
    config_ref_ = config_ref;
    enabled_ = true;
    return true;
  }

  void disable() noexcept {
    enabled_ = false;
    config_ref_ = acquisition::ConfigRef{};
  }

  bool enabled() const noexcept { return enabled_; }
  const acquisition::ConfigRef& config_ref() const noexcept {
    return config_ref_;
  }
  Handoff& handoff() noexcept { return handoff_; }
  const Handoff& handoff() const noexcept { return handoff_; }

 private:
  bool enabled_{false};
  acquisition::ConfigRef config_ref_{};
  Handoff handoff_{};
};

static_assert(sizeof(ProcessingRuntime::Handoff) < 20000U,
              "processing handoff must remain bounded");

}  // namespace sigap
