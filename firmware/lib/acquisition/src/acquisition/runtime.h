#pragma once

#include "acquisition/handoff.h"
#include "acquisition/provenance.h"
#include "acquisition/scheduler.h"

namespace sigap::acquisition {

class AcquisitionFoundation final {
 public:
  static constexpr std::size_t kHandoffCapacity{16};

  explicit AcquisitionFoundation(const config::ConfigManager& config) noexcept
      : config_ref_(active_config_ref(config)), sampling_(config.active().sampling) {}

  void poll(uint64_t monotonic_us) noexcept { scheduler_.poll(monotonic_us); }
  void set_active_config(const config::AppConfig& active, const ConfigRef& ref) noexcept {
    config_ref_ = ref;
    sampling_ = active.sampling;
  }
  bool register_job(uint64_t period_us, CooperativeScheduler::Callback callback,
                    void* context, uint64_t first_due_us) noexcept {
    return scheduler_.add_job(period_us, callback, context, first_due_us);
  }
  const ConfigRef& config_ref() const noexcept { return config_ref_; }
  const config::SamplingConfig& sampling() const noexcept { return sampling_; }
  const SampleRing<kHandoffCapacity>& handoff() const noexcept { return handoff_; }

 private:
  ConfigRef config_ref_{};
  config::SamplingConfig sampling_{};
  CooperativeScheduler scheduler_{};
  SampleRing<kHandoffCapacity> handoff_{};
};

}  // namespace sigap::acquisition
