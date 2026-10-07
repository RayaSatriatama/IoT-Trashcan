#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "acquisition/profiles.h"
#include "acquisition/record.h"
#include "fuzzy/fis.h"
#include "models/model_profiles.h"
#include "processing/contracts.h"
#include "safety/safety.h"

namespace sigap::processing {

enum class WindowStatus : uint8_t { Added, Duplicate, Invalid, Full };

inline bool same_record_identity(const acquisition::SampleRecord& left,
                                 const acquisition::SampleRecord& right) noexcept {
  return left.header.sequence == right.header.sequence &&
         left.header.device_id.present == right.header.device_id.present &&
         left.header.device_id.bytes == right.header.device_id.bytes &&
         left.header.boot_id.present == right.header.boot_id.present &&
         left.header.boot_id.bytes == right.header.boot_id.bytes;
}

template <std::size_t Capacity>
class SampleWindow final {
  static_assert(Capacity > 0U, "sample window capacity must be nonzero");

 public:
  WindowStatus try_add(const acquisition::SampleRecord& sample) noexcept {
    if (!sample.header.device_id.present || !sample.header.boot_id.present) {
      return WindowStatus::Invalid;
    }
    for (std::size_t index = 0U; index < size_; ++index) {
      if (same_record_identity(samples_[index], sample)) {
        return WindowStatus::Duplicate;
      }
    }
    if (size_ == Capacity) {
      overflowed_ = true;
      return WindowStatus::Full;
    }
    samples_[size_] = sample;
    ++size_;
    return WindowStatus::Added;
  }

  constexpr std::size_t size() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0U; }
  constexpr bool overflowed() const noexcept { return overflowed_; }
  static constexpr std::size_t capacity() noexcept { return Capacity; }

  const acquisition::SampleRecord* at(std::size_t index) const noexcept {
    return index < size_ ? &samples_[index] : nullptr;
  }

 private:
  std::array<acquisition::SampleRecord, Capacity> samples_{};
  std::size_t size_{0U};
  bool overflowed_{false};
};

enum class CapacityTarget : uint32_t {
  Organic = 1U,
  Inorganic = 2U,
  B3 = 3U,
};

inline constexpr CapacityTarget capacity_target(
    acquisition::StreamId stream) noexcept {
  switch (stream) {
    case acquisition::StreamId::Organic:
      return CapacityTarget::Organic;
    case acquisition::StreamId::Inorganic:
      return CapacityTarget::Inorganic;
    case acquisition::StreamId::B3:
    case acquisition::StreamId::Ambient:
      return CapacityTarget::B3;
  }
  return CapacityTarget::B3;
}

struct SlotCapacityProfile final {
  acquisition::ProfileRef ref{};
  acquisition::StreamId stream{acquisition::StreamId::Organic};
  float operational_capacity_kg{0.0F};
  bool present{false};

  bool valid_for_normalization() const noexcept {
    return present && ref.present &&
           ref.kind == acquisition::ProfileKind::Capacity &&
           ref.schema_version > 0U &&
           ref.target_id == static_cast<uint32_t>(capacity_target(stream)) &&
           std::isfinite(operational_capacity_kg) &&
           operational_capacity_kg > 0.0F;
  }
};

enum class TemperatureSource : uint8_t {
  None,
  Ds18b20B3,
  Sht31_44,
  Sht31_45,
};

// All values are copied into ProcessingSnapshot at the cycle boundary. This
// structure contains no pointer to a mutable active profile.
struct ProcessingProfiles final {
  bool hx711_present{false};
  acquisition::Hx711Profile hx711{};
  bool ultrasonic_present{false};
  acquisition::UltrasonicProfile ultrasonic{};
  bool ds18b20_present{false};
  acquisition::Ds18b20Profile ds18b20{};
  bool sht31_present{false};
  acquisition::Sht31Profile sht31{};
  SlotCapacityProfile capacity{};
  TemperatureSource temperature_source{TemperatureSource::None};
};

enum class ProcessingSnapshotStatus : uint8_t {
  Captured,
  InvalidConfig,
  InvalidStream,
  InvalidWindow,
};

class ProcessingSnapshot final {
 public:
  static ProcessingSnapshotStatus capture(
      uint64_t started_monotonic_us, uint64_t maximum_sample_age_us,
      acquisition::StreamId stream,
      const acquisition::ConfigRef& config_ref,
      const ProcessingProfiles& profiles,
      ProcessingSnapshot& output) noexcept {
    if (!config_ref.present || config_ref.schema_version == 0U) {
      return ProcessingSnapshotStatus::InvalidConfig;
    }
    if (stream == acquisition::StreamId::Ambient) {
      return ProcessingSnapshotStatus::InvalidStream;
    }
    if (maximum_sample_age_us == 0U) {
      return ProcessingSnapshotStatus::InvalidWindow;
    }
    ProcessingSnapshot candidate{};
    candidate.present_ = true;
    candidate.started_monotonic_us_ = started_monotonic_us;
    candidate.maximum_sample_age_us_ = maximum_sample_age_us;
    candidate.stream_ = stream;
    candidate.config_ref_ = config_ref;
    candidate.profiles_ = profiles;
    output = candidate;
    return ProcessingSnapshotStatus::Captured;
  }

  constexpr bool present() const noexcept { return present_; }
  constexpr uint64_t started_monotonic_us() const noexcept {
    return started_monotonic_us_;
  }
  constexpr uint64_t maximum_sample_age_us() const noexcept {
    return maximum_sample_age_us_;
  }
  constexpr acquisition::StreamId stream() const noexcept { return stream_; }
  constexpr const acquisition::ConfigRef& config_ref() const noexcept {
    return config_ref_;
  }
  constexpr const ProcessingProfiles& profiles() const noexcept {
    return profiles_;
  }

 private:
  bool present_{false};
  uint64_t started_monotonic_us_{0U};
  uint64_t maximum_sample_age_us_{0U};
  acquisition::StreamId stream_{acquisition::StreamId::Organic};
  acquisition::ConfigRef config_ref_{};
  ProcessingProfiles profiles_{};
};

struct ProcessedMetric final {
  bool available{false};
  float value{0.0F};
  InputDisposition disposition{};
};

struct ProcessedValues final {
  ProcessedMetric fill_pct{};
  ProcessedMetric weight_kg{};
  ProcessedMetric weight_utilization_pct{};
  ProcessedMetric ogi{};
  ProcessedMetric accumulation_hours{};
  ProcessedMetric temperature_c{};
};

template <std::size_t SourceCapacity>
struct ProcessedResult final {
  acquisition::StreamId stream{acquisition::StreamId::Organic};
  uint64_t computed_monotonic_us{0U};
  acquisition::ConfigRef config_ref{};
  ProfileRefSet profile_refs{};
  SourceSampleIds<SourceCapacity> source_sample_ids{};
  ProcessedValues processed{};
};

enum class ProcessStatus : uint8_t {
  Produced,
  InvalidSnapshot,
  WindowOverflow,
  NoSources,
  SourceCapacityExceeded,
  ProfileCapacityExceeded,
};

inline bool same_config_ref(const acquisition::ConfigRef& left,
                            const acquisition::ConfigRef& right) noexcept {
  return left.present && right.present &&
         left.schema_version == right.schema_version &&
         left.config_version == right.config_version && left.hash == right.hash;
}

inline bool same_profile_ref(const acquisition::ProfileRef& left,
                             const acquisition::ProfileRef& right) noexcept {
  return left.present && right.present && left.kind == right.kind &&
         left.schema_version == right.schema_version &&
         left.profile_version == right.profile_version &&
         left.target_id == right.target_id && left.hash == right.hash;
}

inline InputDisposition sample_disposition(
    acquisition::SampleQuality quality) noexcept {
  switch (quality) {
    case acquisition::SampleQuality::Ready:
      return {InputQuality::Ready, ProcessingReason::Ready};
    case acquisition::SampleQuality::WarmingUp:
    case acquisition::SampleQuality::HeaterPhase:
    case acquisition::SampleQuality::Uncalibrated:
      return {InputQuality::Uncalibrated,
              ProcessingReason::InputUncalibrated};
    case acquisition::SampleQuality::Calibrating:
      return {InputQuality::Calibrating,
              ProcessingReason::InputCalibrating};
    case acquisition::SampleQuality::Timeout:
      return {InputQuality::Timeout, ProcessingReason::InputTimeout};
    case acquisition::SampleQuality::Disconnected:
      return {InputQuality::Disconnected,
              ProcessingReason::InputDisconnected};
    case acquisition::SampleQuality::CrcError:
      return {InputQuality::CrcError, ProcessingReason::InputCrcError};
    case acquisition::SampleQuality::Stale:
      return {InputQuality::Stale, ProcessingReason::InputStale};
    case acquisition::SampleQuality::OutOfRange:
      return {InputQuality::OutOfRange,
              ProcessingReason::InputOutOfRange};
  }
  return {InputQuality::Missing, ProcessingReason::InputMissing};
}

inline acquisition::SensorId hx_sensor(acquisition::StreamId stream) noexcept {
  switch (stream) {
    case acquisition::StreamId::Organic:
      return acquisition::SensorId::Hx711Organic;
    case acquisition::StreamId::Inorganic:
      return acquisition::SensorId::Hx711Inorganic;
    case acquisition::StreamId::B3:
    case acquisition::StreamId::Ambient:
      return acquisition::SensorId::Hx711B3;
  }
  return acquisition::SensorId::Hx711B3;
}

inline acquisition::SensorId ultrasonic_sensor(
    acquisition::StreamId stream) noexcept {
  switch (stream) {
    case acquisition::StreamId::Organic:
      return acquisition::SensorId::UltrasonicOrganic;
    case acquisition::StreamId::Inorganic:
      return acquisition::SensorId::UltrasonicInorganic;
    case acquisition::StreamId::B3:
    case acquisition::StreamId::Ambient:
      return acquisition::SensorId::UltrasonicB3;
  }
  return acquisition::SensorId::UltrasonicB3;
}

inline bool finite_float(double value) noexcept {
  return std::isfinite(value) &&
         value >= -static_cast<double>(std::numeric_limits<float>::max()) &&
         value <= static_cast<double>(std::numeric_limits<float>::max());
}

template <std::size_t SourceCapacity>
bool add_source(const acquisition::SampleRecord& sample,
                SourceSampleIds<SourceCapacity>& sources) noexcept {
  const SourceSampleId id{sample.header.device_id, sample.header.boot_id,
                          sample.header.sequence};
  const auto status = sources.try_add(id);
  return status == CollectionStatus::Added ||
         status == CollectionStatus::Duplicate;
}

inline bool add_profile(const acquisition::ProfileRef& ref,
                        ProfileRefSet& profiles) noexcept {
  if (!ref.present) return true;
  const auto status = profiles.try_add(ref);
  return status == CollectionStatus::Added ||
         status == CollectionStatus::Duplicate;
}

template <std::size_t WindowCapacity, std::size_t SourceCapacity>
class SlotProcessor final {
 public:
  static ProcessStatus process(
      const SampleWindow<WindowCapacity>& window,
      const ProcessingSnapshot& snapshot,
      ProcessedResult<SourceCapacity>& output) noexcept {
    output = ProcessedResult<SourceCapacity>{};
    if (!snapshot.present()) return ProcessStatus::InvalidSnapshot;
    if (window.overflowed()) return ProcessStatus::WindowOverflow;

    ProcessedResult<SourceCapacity> candidate{};
    candidate.stream = snapshot.stream();
    candidate.computed_monotonic_us = snapshot.started_monotonic_us();
    candidate.config_ref = snapshot.config_ref();
    initialize_unavailable(candidate.processed);

    if (!collect_weight(window, snapshot, candidate) ||
        !collect_fill(window, snapshot, candidate) ||
        !collect_temperature(window, snapshot, candidate)) {
      return ProcessStatus::SourceCapacityExceeded;
    }
    if (!collect_profile_refs(snapshot, candidate.profile_refs)) {
      return ProcessStatus::ProfileCapacityExceeded;
    }
    normalize_weight(snapshot, candidate.processed);
    if (candidate.source_sample_ids.empty()) return ProcessStatus::NoSources;

    output = candidate;
    return ProcessStatus::Produced;
  }

 private:
  static void initialize_unavailable(ProcessedValues& values) noexcept {
    values.fill_pct.disposition = {InputQuality::Missing,
                                   ProcessingReason::InputMissing};
    values.weight_kg.disposition = {InputQuality::Missing,
                                    ProcessingReason::InputMissing};
    values.weight_utilization_pct.disposition = {
        InputQuality::Missing, ProcessingReason::InputMissing};
    values.ogi.disposition = {InputQuality::Missing,
                              ProcessingReason::InputMissing};
    values.accumulation_hours.disposition = {
        InputQuality::Missing, ProcessingReason::InputMissing};
    values.temperature_c.disposition = {InputQuality::Missing,
                                        ProcessingReason::InputMissing};
  }

  static bool sample_is_fresh(const acquisition::SampleRecord& sample,
                              const ProcessingSnapshot& snapshot) noexcept {
    return sample.header.monotonic_us <= snapshot.started_monotonic_us() &&
           snapshot.started_monotonic_us() - sample.header.monotonic_us <=
               snapshot.maximum_sample_age_us();
  }

  static bool sample_matches_cycle(
      const acquisition::SampleRecord& sample,
      const ProcessingSnapshot& snapshot) noexcept {
    return sample.header.stream == snapshot.stream() &&
           same_config_ref(sample.header.config_ref, snapshot.config_ref());
  }

  static bool collect_weight(
      const SampleWindow<WindowCapacity>& window,
      const ProcessingSnapshot& snapshot,
      ProcessedResult<SourceCapacity>& result) noexcept {
    const auto sensor = hx_sensor(snapshot.stream());
    const auto& profiles = snapshot.profiles();
    const acquisition::SampleRecord* latest{nullptr};
    double kilograms_sum{0.0};
    std::size_t valid_count{0U};

    for (std::size_t index = 0U; index < window.size(); ++index) {
      const auto* sample = window.at(index);
      if (sample == nullptr || sample->header.sensor != sensor ||
          sample->header.stream != snapshot.stream()) {
        continue;
      }
      if (latest == nullptr ||
          sample->header.monotonic_us > latest->header.monotonic_us) {
        latest = sample;
      }
      if (!sample_is_fresh(*sample, snapshot) ||
          !sample_matches_cycle(*sample, snapshot) ||
          sample->quality != acquisition::SampleQuality::Ready ||
          !sample->raw.present || !profiles.hx711_present ||
          !profiles.hx711.valid_for_conversion() ||
          !acquisition::profile_matches(profiles.hx711.manifest, sensor) ||
          !same_profile_ref(sample->header.profile_ref,
                            profiles.hx711.ref())) {
        continue;
      }
      const double net_counts =
          static_cast<double>(sample->raw.signed_counts) -
          static_cast<double>(profiles.hx711.tare_offset_counts);
      const double kilograms =
          net_counts / static_cast<double>(profiles.hx711.counts_per_kg);
      if (!finite_float(kilograms)) continue;
      if (!add_source(*sample, result.source_sample_ids)) return false;
      kilograms_sum += kilograms;
      ++valid_count;
    }

    if (valid_count > 0U) {
      const double average = kilograms_sum / static_cast<double>(valid_count);
      if (finite_float(average)) {
        result.processed.weight_kg = {
            true, static_cast<float>(average),
            {InputQuality::Ready, ProcessingReason::Ready}};
        return true;
      }
    }

    if (latest != nullptr) {
      if (!add_source(*latest, result.source_sample_ids)) return false;
      result.processed.weight_kg.disposition =
          unavailable_disposition(*latest, snapshot, profiles.hx711_present,
                                  profiles.hx711.valid_for_conversion(),
                                  profiles.hx711_present
                                      ? profiles.hx711.ref()
                                      : acquisition::ProfileRef{});
    } else if (!profiles.hx711_present) {
      result.processed.weight_kg.disposition = {
          InputQuality::Uncalibrated, ProcessingReason::ProfileMissing};
    }
    return true;
  }

  static bool collect_fill(
      const SampleWindow<WindowCapacity>& window,
      const ProcessingSnapshot& snapshot,
      ProcessedResult<SourceCapacity>& result) noexcept {
    const auto sensor = ultrasonic_sensor(snapshot.stream());
    const auto& profiles = snapshot.profiles();
    const acquisition::SampleRecord* latest{nullptr};
    for (std::size_t index = 0U; index < window.size(); ++index) {
      const auto* sample = window.at(index);
      if (sample != nullptr && sample->header.sensor == sensor &&
          sample->header.stream == snapshot.stream() &&
          (latest == nullptr || sample->header.monotonic_us >
                                    latest->header.monotonic_us)) {
        latest = sample;
      }
    }
    if (latest == nullptr) {
      if (!profiles.ultrasonic_present) {
        result.processed.fill_pct.disposition = {
            InputQuality::Uncalibrated, ProcessingReason::ProfileMissing};
      }
      return true;
    }
    if (!add_source(*latest, result.source_sample_ids)) return false;
    if (!sample_is_fresh(*latest, snapshot) ||
        !sample_matches_cycle(*latest, snapshot) ||
        latest->quality != acquisition::SampleQuality::Ready ||
        !latest->raw.present || !profiles.ultrasonic_present ||
        !profiles.ultrasonic.valid_for_conversion() ||
        !acquisition::profile_matches(profiles.ultrasonic.manifest, sensor) ||
        !same_profile_ref(latest->header.profile_ref,
                          profiles.ultrasonic.ref())) {
      result.processed.fill_pct.disposition = unavailable_disposition(
          *latest, snapshot, profiles.ultrasonic_present,
          profiles.ultrasonic.valid_for_conversion(),
          profiles.ultrasonic_present ? profiles.ultrasonic.ref()
                                      : acquisition::ProfileRef{});
      return true;
    }

    const double distance_mm =
        static_cast<double>(latest->raw.distance_us) *
        static_cast<double>(profiles.ultrasonic.echo_mm_per_us);
    const double minimum =
        static_cast<double>(profiles.ultrasonic.d_full_mm) -
        static_cast<double>(profiles.ultrasonic.overshoot_tolerance_mm);
    const double maximum =
        static_cast<double>(profiles.ultrasonic.d_empty_mm) +
        static_cast<double>(profiles.ultrasonic.overshoot_tolerance_mm);
    if (!std::isfinite(distance_mm) || distance_mm < minimum ||
        distance_mm > maximum) {
      result.processed.fill_pct.disposition = {
          InputQuality::OutOfRange, ProcessingReason::InputOutOfRange};
      return true;
    }
    double fill =
        100.0 * (static_cast<double>(profiles.ultrasonic.d_empty_mm) -
                 distance_mm) /
        (static_cast<double>(profiles.ultrasonic.d_empty_mm) -
         static_cast<double>(profiles.ultrasonic.d_full_mm));
    fill = std::max(0.0, std::min(100.0, fill));
    result.processed.fill_pct = {
        true, static_cast<float>(fill),
        {InputQuality::Ready, ProcessingReason::Ready}};
    return true;
  }

  static bool collect_temperature(
      const SampleWindow<WindowCapacity>& window,
      const ProcessingSnapshot& snapshot,
      ProcessedResult<SourceCapacity>& result) noexcept {
    const auto source = snapshot.profiles().temperature_source;
    if (source == TemperatureSource::None) return true;
    acquisition::SensorId sensor = acquisition::SensorId::Ds18b20B3;
    if (source == TemperatureSource::Sht31_44) {
      sensor = acquisition::SensorId::Sht31_44;
    } else if (source == TemperatureSource::Sht31_45) {
      sensor = acquisition::SensorId::Sht31_45;
    }
    const acquisition::SampleRecord* latest{nullptr};
    for (std::size_t index = 0U; index < window.size(); ++index) {
      const auto* sample = window.at(index);
      if (sample != nullptr && sample->header.sensor == sensor &&
          sample->header.stream == snapshot.stream() &&
          (latest == nullptr || sample->header.monotonic_us >
                                    latest->header.monotonic_us)) {
        latest = sample;
      }
    }
    if (latest == nullptr) return true;
    if (!add_source(*latest, result.source_sample_ids)) return false;

    const bool is_ds = source == TemperatureSource::Ds18b20B3;
    const bool profile_present = is_ds ? snapshot.profiles().ds18b20_present
                                       : snapshot.profiles().sht31_present;
    const bool profile_valid =
        is_ds ? snapshot.profiles().ds18b20.valid_for_conversion()
              : snapshot.profiles().sht31.valid_for_conversion();
    const auto ref =
        is_ds ? snapshot.profiles().ds18b20.ref()
              : snapshot.profiles().sht31.ref();
    const bool raw_valid =
        latest->raw.present && latest->raw.identity_valid &&
        (is_ds ? (latest->raw.conversion_complete && latest->raw.crc_valid)
               : latest->raw.temperature_crc_valid);
    const bool sensor_profile_matches =
        is_ds || snapshot.profiles().sht31.address ==
                     (sensor == acquisition::SensorId::Sht31_45 ? 0x45U
                                                                : 0x44U);
    if (!sample_is_fresh(*latest, snapshot) ||
        !sample_matches_cycle(*latest, snapshot) ||
        latest->quality != acquisition::SampleQuality::Ready || !raw_valid ||
        !profile_present || !profile_valid || !sensor_profile_matches ||
        !acquisition::profile_matches(is_ds
                                          ? snapshot.profiles().ds18b20.manifest
                                          : snapshot.profiles().sht31.manifest,
                                      sensor) ||
        !same_profile_ref(latest->header.profile_ref, ref)) {
      result.processed.temperature_c.disposition = unavailable_disposition(
          *latest, snapshot, profile_present, profile_valid, ref);
      return true;
    }
    const int32_t offset =
        is_ds ? snapshot.profiles().ds18b20.offset_centi_c
              : snapshot.profiles().sht31.temperature_offset_centi_c;
    const double temperature =
        (static_cast<double>(latest->raw.temperature_centi_c) +
         static_cast<double>(offset)) /
        100.0;
    if (!finite_float(temperature)) {
      result.processed.temperature_c.disposition = {
          InputQuality::OutOfRange, ProcessingReason::ConversionFailed};
      return true;
    }
    result.processed.temperature_c = {
        true, static_cast<float>(temperature),
        {InputQuality::Ready, ProcessingReason::Ready}};
    return true;
  }

  static InputDisposition unavailable_disposition(
      const acquisition::SampleRecord& sample,
      const ProcessingSnapshot& snapshot, bool profile_present,
      bool profile_valid,
      const acquisition::ProfileRef& expected_profile) noexcept {
    if (!sample_is_fresh(sample, snapshot)) {
      return {InputQuality::Stale, ProcessingReason::InputStale};
    }
    if (sample.quality != acquisition::SampleQuality::Ready) {
      return sample_disposition(sample.quality);
    }
    if (!profile_present) {
      return {InputQuality::Uncalibrated, ProcessingReason::ProfileMissing};
    }
    if (!profile_valid ||
        !same_profile_ref(sample.header.profile_ref, expected_profile)) {
      return {InputQuality::Uncalibrated, ProcessingReason::ProfileInvalid};
    }
    return {InputQuality::OutOfRange, ProcessingReason::ConversionFailed};
  }

  static void normalize_weight(const ProcessingSnapshot& snapshot,
                               ProcessedValues& values) noexcept {
    if (!values.weight_kg.available) {
      values.weight_utilization_pct.disposition =
          values.weight_kg.disposition;
      return;
    }
    const auto& capacity = snapshot.profiles().capacity;
    if (!capacity.present) {
      values.weight_utilization_pct.disposition = {
          InputQuality::Uncalibrated, ProcessingReason::CapacityMissing};
      return;
    }
    if (!capacity.valid_for_normalization() ||
        capacity.stream != snapshot.stream()) {
      values.weight_utilization_pct.disposition = {
          InputQuality::Uncalibrated, ProcessingReason::CapacityInvalid};
      return;
    }
    const double utilization =
        100.0 * static_cast<double>(values.weight_kg.value) /
        static_cast<double>(capacity.operational_capacity_kg);
    if (!finite_float(utilization)) {
      values.weight_utilization_pct.disposition = {
          InputQuality::OutOfRange, ProcessingReason::ConversionFailed};
      return;
    }
    values.weight_utilization_pct = {
        true, static_cast<float>(utilization),
        {InputQuality::Ready, ProcessingReason::Ready}};
  }

  static bool collect_profile_refs(const ProcessingSnapshot& snapshot,
                                   ProfileRefSet& refs) noexcept {
    const auto& profiles = snapshot.profiles();
    if (profiles.hx711_present &&
        !add_profile(profiles.hx711.ref(), refs)) {
      return false;
    }
    if (profiles.ultrasonic_present &&
        !add_profile(profiles.ultrasonic.ref(), refs)) {
      return false;
    }
    if (profiles.ds18b20_present &&
        !add_profile(profiles.ds18b20.ref(), refs)) {
      return false;
    }
    if (profiles.sht31_present &&
        !add_profile(profiles.sht31.ref(), refs)) {
      return false;
    }
    return add_profile(profiles.capacity.ref, refs);
  }
};

inline fuzzy::FisInputState to_fis_input_state(
    const ProcessedMetric& metric) noexcept {
  if (metric.available) return fuzzy::FisInputState::Ready;
  switch (metric.disposition.quality) {
    case InputQuality::Stale: return fuzzy::FisInputState::Stale;
    case InputQuality::Calibrating: return fuzzy::FisInputState::Calibrating;
    case InputQuality::Uncalibrated:
      return fuzzy::FisInputState::Uncalibrated;
    case InputQuality::Missing: return fuzzy::FisInputState::Missing;
    default: return fuzzy::FisInputState::Invalid;
  }
}

inline const ProcessedMetric* metric_for_input(
    const ProcessedValues& values, models::ModelInput input) noexcept {
  switch (input) {
    case models::ModelInput::FillPercent: return &values.fill_pct;
    case models::ModelInput::WeightUtilizationPercent:
      return &values.weight_utilization_pct;
    case models::ModelInput::TemperatureC: return &values.temperature_c;
    case models::ModelInput::Mq7AdcMillivolts:
    case models::ModelInput::Mq8AdcMillivolts:
      return nullptr;
  }
  return nullptr;
}

template <typename FuzzyResult, typename SafetyResult,
          std::size_t SourceCapacity>
struct EvaluatedResult;

template <std::size_t WindowCapacity, std::size_t SourceCapacity>
class ProcessingPipeline final {
 public:
  using FuzzyResult = fuzzy::FisResult;
  using SafetyResult = safety::SafetyResult;
  using Result = EvaluatedResult<FuzzyResult, SafetyResult, SourceCapacity>;

  static ProcessStatus evaluate(
      const SampleWindow<WindowCapacity>& window,
      const ProcessingSnapshot& snapshot,
      const models::FisModelProfile* fis_profile,
      const models::SafetyModelProfile* safety_profile,
      Result& output) noexcept {
    safety::SafetyEvaluator evaluator{};
    return evaluate(window, snapshot, fis_profile, safety_profile, evaluator,
                    output);
  }

  static ProcessStatus evaluate(
      const SampleWindow<WindowCapacity>& window,
      const ProcessingSnapshot& snapshot,
      const models::FisModelProfile* fis_profile,
      const models::SafetyModelProfile* safety_profile,
      safety::SafetyEvaluator& safety_evaluator,
      Result& output) noexcept {
    ProcessedResult<SourceCapacity> processed{};
    const auto process_status =
        SlotProcessor<WindowCapacity, SourceCapacity>::process(
            window, snapshot, processed);
    if (process_status != ProcessStatus::Produced) {
      output = Result{};
      return process_status;
    }

    if (fis_profile != nullptr) {
      const auto status = processed.profile_refs.try_add(fis_profile->ref());
      if (status != CollectionStatus::Added &&
          status != CollectionStatus::Duplicate) {
        output = Result{};
        return ProcessStatus::ProfileCapacityExceeded;
      }
    }
    if (safety_profile != nullptr) {
      const auto status =
          processed.profile_refs.try_add(safety_profile->ref());
      if (status != CollectionStatus::Added &&
          status != CollectionStatus::Duplicate) {
        output = Result{};
        return ProcessStatus::ProfileCapacityExceeded;
      }
    }

    fuzzy::FisInputs fis_inputs{};
    if (fis_profile != nullptr) {
      fis_inputs.count = fis_profile->input_count;
      if (fis_inputs.count > fis_inputs.values.size()) {
        output = Result{};
        return ProcessStatus::InvalidSnapshot;
      }
      for (std::size_t index = 0U; index < fis_inputs.count; ++index) {
        const auto* metric = metric_for_input(
            processed.processed, fis_profile->inputs[index].id);
        fis_inputs.values[index] =
            metric == nullptr
                ? fuzzy::FisInput{fuzzy::FisInputState::Missing, 0.0F}
                : fuzzy::FisInput{to_fis_input_state(*metric), metric->value};
      }
    }
    const models::FisModelProfile empty_fis{};
    const auto& fis = fis_profile != nullptr ? *fis_profile : empty_fis;
    const auto fuzzy_result = fuzzy::MamdaniEvaluator::evaluate(fis, fis_inputs);

    safety::HazardInputFrame hazard_inputs{};
    if (safety_profile != nullptr) {
      if (safety_profile->channel_count > hazard_inputs.values.size()) {
        output = Result{};
        return ProcessStatus::InvalidSnapshot;
      }
      for (std::size_t index = 0U; index < safety_profile->channel_count;
           ++index) {
        const auto signal = safety_profile->channels[index].signal;
        const ProcessedMetric* metric = nullptr;
        if (signal == models::HazardSignal::TemperatureC) {
          metric = &processed.processed.temperature_c;
        }
        auto& hazard = hazard_inputs.values[hazard_inputs.count++];
        hazard.signal = signal;
        hazard.value = metric == nullptr ? 0.0F : metric->value;
        hazard.state = metric != nullptr && metric->available
                           ? safety::HazardInputState::Ready
                           : safety::HazardInputState::Missing;
      }
    }
    const auto safety_result =
        safety_evaluator.evaluate(safety_profile, hazard_inputs);
    output = Result{processed, fuzzy_result, safety_result};
    return ProcessStatus::Produced;
  }
};

template <typename FuzzyResult, typename SafetyResult,
          std::size_t SourceCapacity>
struct EvaluatedResult final {
  static_assert(std::is_trivially_copyable<FuzzyResult>::value,
                "FIS result must have bounded value semantics");
  static_assert(std::is_trivially_copyable<SafetyResult>::value,
                "Safety result must have bounded value semantics");

  ProcessedResult<SourceCapacity> processing{};
  FuzzyResultAdapterSlot<FuzzyResult, sizeof(FuzzyResult)> fuzzy{FuzzyResult{}};
  SafetyResultAdapterSlot<SafetyResult, sizeof(SafetyResult)> safety{
      SafetyResult{}};

  constexpr EvaluatedResult() noexcept = default;
  constexpr EvaluatedResult(const ProcessedResult<SourceCapacity>& processed,
                            const FuzzyResult& fuzzy_result,
                            const SafetyResult& safety_result) noexcept
      : processing(processed),
        fuzzy(fuzzy_result),
        safety(safety_result) {}
};

struct ResultRecordContext final {
  acquisition::DeviceId device_id{};
  acquisition::BootId boot_id{};
  acquisition::ConfigRef config_ref{};
  acquisition::IUtcSource* utc_source{nullptr};
};

struct ResultRecordHeader final {
  acquisition::DeviceId device_id{};
  acquisition::BootId boot_id{};
  uint64_t sequence{0U};
  uint64_t monotonic_us{0U};
  acquisition::RecordedAt recorded_at{};
  acquisition::TimeQuality time_quality{acquisition::TimeQuality::Unsynced};
  acquisition::ConfigRef config_ref{};
};

template <typename FuzzyResult, typename SafetyResult,
          std::size_t SourceCapacity>
struct ResultEnvelope final {
  ResultRecordHeader header{};
  EvaluatedResult<FuzzyResult, SafetyResult, SourceCapacity> result{};
};

enum class ResultFactoryStatus : uint8_t {
  Produced,
  InvalidContext,
  InvalidResult,
  ProvenanceMismatch,
  SequenceExhausted,
};

// The allocator is injected so sample, result, event, and metadata producers
// can consume one boot-local sequence domain. This factory does not own or
// reset the allocator.
class ResultRecordFactory final {
 public:
  ResultRecordFactory(acquisition::SequenceAllocator& sequence,
                      const ResultRecordContext& context) noexcept
      : sequence_(sequence), context_(context) {}

  template <typename FuzzyResult, typename SafetyResult,
            std::size_t SourceCapacity>
  ResultFactoryStatus make(
      uint64_t monotonic_us,
      const EvaluatedResult<FuzzyResult, SafetyResult, SourceCapacity>& result,
      ResultEnvelope<FuzzyResult, SafetyResult, SourceCapacity>& output) noexcept {
    if (!context_.device_id.present || !context_.boot_id.present ||
        !context_.config_ref.present ||
        context_.config_ref.schema_version == 0U) {
      return ResultFactoryStatus::InvalidContext;
    }
    if (result.processing.source_sample_ids.empty() ||
        result.processing.source_sample_ids.overflowed() ||
        result.processing.profile_refs.overflowed() ||
        result.processing.computed_monotonic_us > monotonic_us) {
      return ResultFactoryStatus::InvalidResult;
    }
    if (!same_config_ref(result.processing.config_ref,
                         context_.config_ref)) {
      return ResultFactoryStatus::ProvenanceMismatch;
    }

    uint64_t sequence{0U};
    if (!sequence_.allocate(sequence)) {
      return ResultFactoryStatus::SequenceExhausted;
    }

    ResultEnvelope<FuzzyResult, SafetyResult, SourceCapacity> candidate{};
    candidate.header.device_id = context_.device_id;
    candidate.header.boot_id = context_.boot_id;
    candidate.header.sequence = sequence;
    candidate.header.monotonic_us = monotonic_us;
    candidate.header.config_ref = context_.config_ref;
    candidate.header.time_quality = acquisition::TimeQuality::Unsynced;
    if (context_.utc_source != nullptr) {
      int64_t epoch_ms{0};
      if (context_.utc_source->now_ms(epoch_ms)) {
        candidate.header.recorded_at.available = true;
        candidate.header.recorded_at.epoch_ms = epoch_ms;
        candidate.header.time_quality =
            acquisition::TimeQuality::Synchronized;
      }
    }
    candidate.result = result;
    output = candidate;
    return ResultFactoryStatus::Produced;
  }

 private:
  acquisition::SequenceAllocator& sequence_;
  ResultRecordContext context_{};
};

enum class ResultHandoffStatus : uint8_t { Enqueued, Dequeued, Empty, Full };

template <typename Result, std::size_t Capacity>
class EvaluatedResultRing final {
  static_assert(Capacity > 0U, "result handoff capacity must be nonzero");

 public:
  ResultHandoffStatus try_push(const Result& result) noexcept {
    if (size_ == Capacity) {
      ++dropped_results_;
      return ResultHandoffStatus::Full;
    }
    results_[tail_] = result;
    tail_ = (tail_ + 1U) % Capacity;
    ++size_;
    return ResultHandoffStatus::Enqueued;
  }

  ResultHandoffStatus try_pop(Result& result) noexcept {
    if (size_ == 0U) return ResultHandoffStatus::Empty;
    result = results_[head_];
    head_ = (head_ + 1U) % Capacity;
    --size_;
    return ResultHandoffStatus::Dequeued;
  }

  constexpr std::size_t size() const noexcept { return size_; }
  constexpr uint64_t dropped_results() const noexcept {
    return dropped_results_;
  }

 private:
  std::array<Result, Capacity> results_{};
  std::size_t head_{0U};
  std::size_t tail_{0U};
  std::size_t size_{0U};
  uint64_t dropped_results_{0U};
};

}  // namespace sigap::processing
