#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <limits>

#include "acquisition/record.h"
#include "acquisition/target_runtime.h"
#include "config/config_types.h"
#include "fuzzy/fis.h"
#include "models/model_profiles.h"
#include "processing/processing_engine.h"
#include "safety/safety.h"
#include "sigap/config/config_defaults.h"

namespace sigap {

// Application-owned, non-durable processing boundary. Acquisition calls
// observe() in addition to its logger-facing sample handoff, so processing
// never pops or acknowledges a sample record that a future logger needs.
class ProcessingRuntime final : public acquisition::ISampleObserver {
 public:
  static constexpr std::size_t kWindowCapacity{16U};
  // One retained attempt per acquisition channel: 3 HX711, 3 ultrasonic,
  // 4 MQ, DS18B20, and two SHT31 channels.
  static constexpr std::size_t kChannelCapacity{13U};
  static_assert(kChannelCapacity <= kWindowCapacity,
                "processing retention must fit the processing window");
  static constexpr std::size_t kSourceCapacity{16U};
  static constexpr std::size_t kResultHandoffCapacity{3U};

  using Pipeline = processing::ProcessingPipeline<kWindowCapacity,
                                                  kSourceCapacity>;
  using Result = typename Pipeline::Result;
  using ResultEnvelope = processing::ResultEnvelope<
      fuzzy::FisResult, safety::SafetyResult, kSourceCapacity>;
  using Handoff = processing::EvaluatedResultRing<ResultEnvelope,
                                                   kResultHandoffCapacity>;

  bool begin(const config::AppConfig& active,
             const acquisition::ConfigRef& config_ref,
             acquisition::SequenceAllocator& sequence,
             const processing::ResultRecordContext& context,
             const acquisition::TargetProfileSet& profiles,
             uint64_t first_due_us) noexcept {
    disable();
    (void)active;
    const uint64_t period_us = processing_policy::kProcessingPeriodUs;
    if (!config_ref.present || config_ref.schema_version == 0U ||
        !processing::same_config_ref(config_ref, context.config_ref) ||
        !context.device_id.present || !context.boot_id.present ||
        period_us == 0U) {
      return false;
    }

    config_ref_ = config_ref;
    record_context_ = context;
    sequence_ = &sequence;
    profiles_ = &profiles;
    period_us_ = period_us;
    next_due_us_ = first_due_us;
    retained_count_ = 0U;
    enabled_ = true;
    return true;
  }

  void disable() noexcept {
    enabled_ = false;
    config_ref_ = acquisition::ConfigRef{};
    record_context_ = processing::ResultRecordContext{};
    sequence_ = nullptr;
    profiles_ = nullptr;
    fis_profiles_.fill(nullptr);
    safety_profile_ = nullptr;
    period_us_ = 0U;
    next_due_us_ = 0U;
    window_ = processing::SampleWindow<kWindowCapacity>{};
    retained_count_ = 0U;
    safety_evaluator_.reset();
  }

  // Calibration evidence receives every acquisition attempt too. The
  // downstream observer is independent from the processing window and from
  // the logger-facing acquisition ring.
  void set_downstream_observer(
      acquisition::ISampleObserver* observer) noexcept {
    downstream_observer_ = observer;
  }

  // Model pointers are captured by value at each due cycle. A null pointer is
  // intentionally meaningful: the resulting FIS/safety state is
  // UNCONFIGURED, not a synthetic model.
  void set_model_profiles(const models::FisModelProfile* fis,
                          const models::SafetyModelProfile* safety) noexcept {
    fis_profiles_.fill(fis);
    safety_profile_ = safety;
  }

  void set_model_profiles(
      const std::array<const models::FisModelProfile*, 3U>& fis,
      const models::SafetyModelProfile* safety) noexcept {
    fis_profiles_ = fis;
    safety_profile_ = safety;
  }

  void observe(const acquisition::SampleRecord& sample) noexcept override {
    if (downstream_observer_ != nullptr) {
      downstream_observer_->observe(sample);
    }
    if (!enabled_) return;
    if (!sample_matches_runtime(sample)) {
      ++rejected_samples_;
      return;
    }
    const auto status = retain_latest(sample);
    if (status == RetainedStatus::Added ||
        status == RetainedStatus::Replaced) {
      ++observed_samples_;
    } else if (status == RetainedStatus::Full) {
      ++dropped_samples_;
    } else if (status == RetainedStatus::Invalid ||
               status == RetainedStatus::Older) {
      ++rejected_samples_;
    }
  }

  void poll(uint64_t now_us) noexcept {
    if (!enabled_ || sequence_ == nullptr || profiles_ == nullptr ||
        now_us < next_due_us_) {
      return;
    }

    advance_due(now_us);
    // A missed deadline is a scheduling fact, not a future sample timestamp.
    // Capture the cycle at the observed monotonic time so records collected
    // after the old deadline remain eligible rather than being future-dated.
    evaluate_cycle(now_us);
    // Keep the latest attempt between cycles: slower channels may still be
    // fresh. Quality, age, and profile identity are checked on every cycle.
    // begin()/disable() clear this cache; a newer failed attempt replaces READY.
    window_ = processing::SampleWindow<kWindowCapacity>{};
    ++cycles_completed_;
  }

  bool enabled() const noexcept { return enabled_; }
  const acquisition::ConfigRef& config_ref() const noexcept {
    return config_ref_;
  }
  const Handoff& handoff() const noexcept { return handoff_; }
  Handoff& handoff() noexcept { return handoff_; }
  uint64_t observed_samples() const noexcept { return observed_samples_; }
  uint64_t rejected_samples() const noexcept { return rejected_samples_; }
  uint64_t dropped_samples() const noexcept { return dropped_samples_; }
  uint64_t cycles_completed() const noexcept { return cycles_completed_; }
  uint64_t result_failures() const noexcept { return result_failures_; }
  uint64_t processing_period_us() const noexcept { return period_us_; }
  std::size_t retained_samples() const noexcept { return retained_count_; }

 private:
  static constexpr std::size_t stream_index(
      acquisition::StreamId stream) noexcept {
    switch (stream) {
      case acquisition::StreamId::Organic: return 0U;
      case acquisition::StreamId::Inorganic: return 1U;
      case acquisition::StreamId::B3: return 2U;
      case acquisition::StreamId::Ambient: return 3U;
    }
    return 3U;
  }

  enum class RetainedStatus : uint8_t {
    Added,
    Replaced,
    Older,
    Full,
    Invalid,
  };

  static bool same_channel(const acquisition::SampleRecord& left,
                           const acquisition::SampleRecord& right) noexcept {
    return left.header.sensor == right.header.sensor &&
           left.header.stream == right.header.stream;
  }

  RetainedStatus retain_latest(
      const acquisition::SampleRecord& sample) noexcept {
    if (!sample.header.device_id.present || !sample.header.boot_id.present) {
      return RetainedStatus::Invalid;
    }
    for (std::size_t index = 0U; index < retained_count_; ++index) {
      if (!same_channel(retained_[index], sample)) continue;
      if (sample.header.sequence < retained_[index].header.sequence ||
          sample.header.monotonic_us < retained_[index].header.monotonic_us) {
        return RetainedStatus::Older;
      }
      retained_[index] = sample;
      return RetainedStatus::Replaced;
    }
    if (retained_count_ == retained_.size()) return RetainedStatus::Full;
    retained_[retained_count_++] = sample;
    return RetainedStatus::Added;
  }

  bool sample_matches_runtime(
      const acquisition::SampleRecord& sample) const noexcept {
    return sample.header.device_id.present && sample.header.boot_id.present &&
           sample.header.device_id.bytes == record_context_.device_id.bytes &&
           sample.header.boot_id.bytes == record_context_.boot_id.bytes &&
           processing::same_config_ref(sample.header.config_ref, config_ref_);
  }

  static processing::ProcessingProfiles capture_profiles(
      acquisition::StreamId stream,
      const acquisition::TargetProfileSet& source) noexcept {
    processing::ProcessingProfiles profiles{};
    const std::size_t index = stream_index(stream);
    if (index < 3U && source.hx711[index] != nullptr) {
      profiles.hx711 = *source.hx711[index];
      profiles.hx711_present = true;
    }
    if (index < 3U && source.ultrasonic[index] != nullptr) {
      profiles.ultrasonic = *source.ultrasonic[index];
      profiles.ultrasonic_present = true;
    }
    if (stream == acquisition::StreamId::B3 && source.ds18b20 != nullptr) {
      profiles.ds18b20 = *source.ds18b20;
      profiles.ds18b20_present = true;
      profiles.temperature_source = processing::TemperatureSource::Ds18b20B3;
    }
    if (stream == acquisition::StreamId::Organic && source.sht31[0] != nullptr) {
      profiles.sht31 = *source.sht31[0];
      profiles.sht31_present = true;
      profiles.temperature_source = processing::TemperatureSource::Sht31_44;
    }
    if (stream == acquisition::StreamId::B3 && source.sht31[1] != nullptr &&
        !profiles.ds18b20_present) {
      profiles.sht31 = *source.sht31[1];
      profiles.sht31_present = true;
      profiles.temperature_source = processing::TemperatureSource::Sht31_45;
    }
    if (index < 3U && source.capacity[index] != nullptr) {
      profiles.capacity = *source.capacity[index];
    }
    for (std::size_t mq_index = 0U; mq_index < source.mq_adc.size();
         ++mq_index) {
      if (source.mq_adc[mq_index] != nullptr) {
        profiles.mq_adc[mq_index] = *source.mq_adc[mq_index];
        profiles.mq_adc_present[mq_index] = true;
      }
    }
    return profiles;
  }

  void evaluate_cycle(uint64_t cycle_started_us) noexcept {
    // Materialize the bounded latest-per-channel retention cache into the
    // processing window only at cycle evaluation. This keeps every acquired
    // record available to the logger observer while processing retains a
    // deterministic one-attempt view for each channel.
    window_ = processing::SampleWindow<kWindowCapacity>{};
    for (std::size_t index = 0U; index < retained_count_; ++index) {
      (void)window_.try_add(retained_[index]);
    }
    if (window_.empty()) return;

    for (const auto stream : {acquisition::StreamId::Organic,
                              acquisition::StreamId::Inorganic,
                              acquisition::StreamId::B3}) {
      processing::ProcessingSnapshot snapshot{};
      const auto snapshot_status = processing::ProcessingSnapshot::capture(
          cycle_started_us, kMaximumSampleAgeUs, stream, config_ref_,
          capture_profiles(stream, *profiles_), snapshot);
      if (snapshot_status != processing::ProcessingSnapshotStatus::Captured) {
        ++result_failures_;
        continue;
      }

      models::FisModelProfile fis_snapshot{};
      models::SafetyModelProfile safety_snapshot{};
      const models::FisModelProfile* fis = nullptr;
      const models::SafetyModelProfile* safety = nullptr;
      const std::size_t stream_slot = stream_index(stream);
      const auto* cycle_fis = stream_slot < fis_profiles_.size()
                                  ? fis_profiles_[stream_slot]
                                  : nullptr;
      if (cycle_fis != nullptr) {
        fis_snapshot = *cycle_fis;
        fis = &fis_snapshot;
      }
      if (safety_profile_ != nullptr && stream == acquisition::StreamId::B3) {
        safety_snapshot = *safety_profile_;
        safety = &safety_snapshot;
      }

      Result result{};
      const auto status = Pipeline::evaluate(window_, snapshot, fis, safety,
                                             safety_evaluator_, result);
      if (status == processing::ProcessStatus::NoSources) continue;
      if (status != processing::ProcessStatus::Produced) {
        ++result_failures_;
        continue;
      }

      processing::ResultRecordFactory factory(*sequence_, record_context_);
      ResultEnvelope envelope{};
      if (factory.make(cycle_started_us, result, envelope) !=
          processing::ResultFactoryStatus::Produced) {
        ++result_failures_;
        continue;
      }
      if (handoff_.try_push(envelope) !=
          processing::ResultHandoffStatus::Enqueued) {
        ++result_failures_;
      }
    }
  }

  void advance_due(uint64_t now_us) noexcept {
    if (period_us_ == 0U || now_us < next_due_us_) return;
    const uint64_t elapsed = now_us - next_due_us_;
    const uint64_t periods = (elapsed / period_us_) + 1U;
    if (periods > (std::numeric_limits<uint64_t>::max() - next_due_us_) /
                      period_us_) {
      next_due_us_ = std::numeric_limits<uint64_t>::max();
      return;
    }
    next_due_us_ += periods * period_us_;
  }

  static constexpr uint64_t kMaximumSampleAgeUs{5000000U};

  bool enabled_{false};
  acquisition::ConfigRef config_ref_{};
  processing::ResultRecordContext record_context_{};
  acquisition::SequenceAllocator* sequence_{nullptr};
  const acquisition::TargetProfileSet* profiles_{nullptr};
  acquisition::ISampleObserver* downstream_observer_{nullptr};
  std::array<const models::FisModelProfile*, 3U> fis_profiles_{};
  const models::SafetyModelProfile* safety_profile_{nullptr};
  processing::SampleWindow<kWindowCapacity> window_{};
  std::array<acquisition::SampleRecord, kChannelCapacity> retained_{};
  std::size_t retained_count_{0U};
  safety::SafetyEvaluator safety_evaluator_{};
  Handoff handoff_{};
  uint64_t period_us_{0U};
  uint64_t next_due_us_{0U};
  uint64_t observed_samples_{0U};
  uint64_t rejected_samples_{0U};
  uint64_t dropped_samples_{0U};
  uint64_t cycles_completed_{0U};
  uint64_t result_failures_{0U};
};

static_assert(sizeof(ProcessingRuntime::Handoff) < 20000U,
              "processing handoff must remain bounded");

}  // namespace sigap
