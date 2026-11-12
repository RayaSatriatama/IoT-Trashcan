#pragma once

#include "acquisition/target_runtime.h"
#include "acquisition/provenance.h"
#include "config/config_manager.h"

namespace sigap::acquisition {

class AcquisitionFoundation final {
 public:
  static constexpr std::size_t kHandoffCapacity{TargetAcquisition::kHandoffCapacity};

  AcquisitionFoundation(const config::ConfigManager& config,
                        RecordFactory& record_factory,
                        const board::BoardPins& pins,
                        TargetProfileSet& profiles) noexcept
      : config_(config),
        config_ref_(active_config_ref(config)),
        sampling_(config.active().sampling),
        record_factory_(record_factory),
        target_(record_factory_, pins, profiles, handoff_) {}

  void set_sample_observer(ISampleObserver* observer) noexcept {
    target_.set_sample_observer(observer);
  }
  void bind_debug_sink(debug::IEventSink* sink) noexcept {
    target_.bind_debug_sink(sink);
  }
  void set_calibration_state_provider(
      const ICalibrationStateProvider* provider) noexcept {
    target_.set_calibration_state_provider(provider);
  }

  bool begin(const config::AppConfig& active, const ConfigRef& ref,
             uint64_t first_due_us) noexcept {
    disable();
    if (!config_.runtime_ready() || !ref.present) return false;
    config_ref_ = ref;
    sampling_ = active.sampling;
    record_factory_.set_config_ref(ref);
    enabled_ = target_.begin(sampling_, first_due_us);
    return enabled_;
  }

  void disable() noexcept {
    enabled_ = false;
    target_.disable();
  }

  void poll(uint64_t monotonic_us) noexcept {
    if (enabled_) target_.poll(monotonic_us);
  }

  bool refresh_profiles(uint64_t first_due_us) noexcept {
    return enabled_ && config_.runtime_ready() && config_ref_.present &&
           target_.refresh_profiles(first_due_us);
  }

  bool enabled() const noexcept { return enabled_; }
  std::size_t registered_jobs() const noexcept { return target_.registered_jobs(); }
  const ConfigRef& config_ref() const noexcept { return config_ref_; }
  const config::SamplingConfig& sampling() const noexcept { return sampling_; }
  const SampleRing<kHandoffCapacity>& handoff() const noexcept { return handoff_; }
  SampleRing<kHandoffCapacity>& handoff() noexcept { return handoff_; }
  const TargetAcquisition& target() const noexcept { return target_; }

 private:
  const config::ConfigManager& config_;
  ConfigRef config_ref_{};
  config::SamplingConfig sampling_{};
  RecordFactory& record_factory_;
  SampleRing<kHandoffCapacity> handoff_{};
  TargetAcquisition target_;
  bool enabled_{false};
};

}  // namespace sigap::acquisition
