#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include <unity.h>

#include "../../../src/app/processing_runtime.h"
#include "acquisition/handoff.h"
#include "models/model_lifecycle.h"
#include "sigap/config/config_defaults.h"
#include "debug/service.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;

ConfigRef active_config_ref() {
  ConfigRef ref{};
  ref.present = true;
  ref.schema_version = 1U;
  ref.config_version = 7U;
  ref.hash[0] = 0xA7U;
  return ref;
}

ProfileManifest profile_manifest(ProfileKind kind, ProfileTarget target) {
  ProfileManifest manifest{};
  manifest.kind = kind;
  manifest.schema_version = 1U;
  manifest.profile_version = 1U;
  manifest.target_id = static_cast<uint32_t>(target);
  manifest.validity = ProfileValidity::Valid;
  manifest.hash[0] = 0x31U;
  return manifest;
}

DeviceId device_id() {
  DeviceId id{};
  id.present = true;
  id.bytes[0] = 0xD1U;
  return id;
}

BootId boot_id() {
  BootId id{};
  id.present = true;
  id.bytes[0] = 0xB1U;
  return id;
}

SampleRecord organic_attempt(uint64_t sequence, uint64_t monotonic_us) {
  SampleRecord record{};
  record.header.device_id = device_id();
  record.header.boot_id = boot_id();
  record.header.sequence = sequence;
  record.header.monotonic_us = monotonic_us;
  record.header.sensor = SensorId::Hx711Organic;
  record.header.stream = StreamId::Organic;
  record.header.kind = SampleKind::Hx711Counts;
  record.header.config_ref = active_config_ref();
  record.quality = SampleQuality::Uncalibrated;
  record.reason = SampleReason::ProfileMissing;
  return record;
}

struct CountingObserver final : ISampleObserver {
  void observe(const SampleRecord&) noexcept override { ++count; }

  uint32_t count{0U};
};

struct ProcessingSink final : debug::IEventSink {
  bool accept{true};
  unsigned attempts{};
  std::array<debug::EventInput, 32U> events{};
  std::size_t count{};
  bool emit(const debug::EventInput& event) noexcept override {
    ++attempts;
    if (!accept) return false;
    if (count < events.size()) events[count++] = event;
    return true;
  }
};

void initialize_runtime(ProcessingRuntime& runtime,
                        SequenceAllocator& sequence,
                        TargetProfileSet& profiles,
                        CountingObserver& downstream) {
  config::AppConfig active = config::kFactoryDefaults;
  active.telemetry.summary_period_ms = 5000U;
  processing::ResultRecordContext context{};
  context.device_id = device_id();
  context.boot_id = boot_id();
  context.config_ref = active_config_ref();
  runtime.set_downstream_observer(&downstream);
  TEST_ASSERT_TRUE(runtime.begin(active, active_config_ref(), sequence, context,
                                 profiles, 0U));
}

void test_availability_uses_real_cycle_reason_and_keeps_acquisition_record() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  ProcessingSink sink{};
  runtime.bind_debug_sink(&sink);
  initialize_runtime(runtime, sequence, profiles, downstream);
  runtime.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.count);

  const auto sample = organic_attempt(50U, 1000000U);
  SampleRing<2U> handoff{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(handoff.try_push(sample)));
  runtime.observe(sample);
  const auto sequence_before = sequence.next_value();
  runtime.poll(1000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
  TEST_ASSERT_EQUAL_UINT16(42U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT32(4, sink.events[0].arg1);
  TEST_ASSERT_EQUAL_UINT8(0U, sink.events[0].presence);
  TEST_ASSERT_EQUAL_UINT64(1000000U, sink.events[0].monotonic_us);
  TEST_ASSERT_EQUAL_UINT64(sequence_before + 1U, sequence.next_value());
  TEST_ASSERT_EQUAL_UINT32(1U, runtime.handoff().size());
  TEST_ASSERT_EQUAL_UINT32(1U, downstream.count);
  TEST_ASSERT_EQUAL_UINT32(1U, handoff.size());
  SampleRecord retained{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                        static_cast<int>(handoff.try_peek(retained)));
  TEST_ASSERT_EQUAL_UINT64(sample.header.sequence, retained.header.sequence);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sample.quality),
                        static_cast<int>(retained.quality));
  runtime.poll(2000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
  auto timed_out = organic_attempt(51U, 3000000U);
  timed_out.quality = SampleQuality::Timeout;
  timed_out.reason = SampleReason::Timeout;
  runtime.observe(timed_out);
  runtime.poll(3000000U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT32(5, sink.events[1].arg1);
}

struct ProcessingDebugLock final : debug::IDebugLock {
  unsigned locks{};
  void lock() noexcept override { ++locks; }
  void unlock() noexcept override {}
};

struct ReadyCycle final {
  std::array<Hx711Profile, 3U> hx{};
  std::array<UltrasonicProfile, 3U> ultrasonic{};
  std::array<processing::SlotCapacityProfile, 3U> capacity{};
  std::array<Sht31Profile, 2U> sht{};
  std::array<MqAdcProfile, 2U> mq{};
  TargetProfileSet profiles{};
  SequenceAllocator sequence;
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  const std::array<StreamId, 3U> streams{{StreamId::Organic,
      StreamId::Inorganic, StreamId::B3}};
  const std::array<SensorId, 3U> hx_sensors{{SensorId::Hx711Organic,
      SensorId::Hx711Inorganic, SensorId::Hx711B3}};
  const std::array<SensorId, 3U> us_sensors{{SensorId::UltrasonicOrganic,
      SensorId::UltrasonicInorganic, SensorId::UltrasonicB3}};

  explicit ReadyCycle(uint64_t first_sequence = 0U) : sequence(first_sequence) {
    const std::array<ProfileTarget, 3U> hx_targets{{ProfileTarget::Hx711Organic,
        ProfileTarget::Hx711Inorganic, ProfileTarget::Hx711B3}};
    const std::array<ProfileTarget, 3U> us_targets{{ProfileTarget::UltrasonicOrganic,
        ProfileTarget::UltrasonicInorganic, ProfileTarget::UltrasonicB3}};
    for (std::size_t i = 0U; i < streams.size(); ++i) {
      hx[i].manifest = profile_manifest(ProfileKind::Calibration, hx_targets[i]);
      hx[i].counts_per_kg = 500.0F;
      hx[i].tare_offset_counts = 1000;
      ultrasonic[i].manifest = profile_manifest(ProfileKind::Calibration, us_targets[i]);
      ultrasonic[i].d_empty_mm = 1000.0F;
      ultrasonic[i].d_full_mm = 200.0F;
      ultrasonic[i].echo_mm_per_us = 0.1F;
      ultrasonic[i].overshoot_tolerance_mm = 10.0F;
      ultrasonic[i].echo_timeout_us = 30000U;
      ultrasonic[i].inter_sensor_gap_us = 60000U;
      capacity[i].manifest = profile_manifest(ProfileKind::Capacity, hx_targets[i]);
      capacity[i].present = true;
      capacity[i].stream = streams[i];
      capacity[i].operational_capacity_kg = 4.0F;
      profiles.hx711[i] = &hx[i];
      profiles.ultrasonic[i] = &ultrasonic[i];
      profiles.capacity[i] = &capacity[i];
    }
    sht[0].manifest = profile_manifest(ProfileKind::Calibration, ProfileTarget::Sht31_44);
    sht[1].manifest = profile_manifest(ProfileKind::Calibration, ProfileTarget::Sht31_45);
    sht[0].address = 0x44U;
    sht[1].address = 0x45U;
    profiles.sht31[0] = &sht[0];
    profiles.sht31[1] = &sht[1];
    mq[0].manifest = profile_manifest(ProfileKind::Adc, ProfileTarget::Mq7);
    mq[1].manifest = profile_manifest(ProfileKind::Adc, ProfileTarget::Mq8);
    for (std::size_t i = 0U; i < mq.size(); ++i) {
      mq[i].reference_mv = 3300U;
      mq[i].full_scale_counts = 4095U;
      mq[i].divider_numerator = 1U;
      mq[i].divider_denominator = 1U;
      profiles.mq_adc[i + 2U] = &mq[i];
    }
  }

  void begin(debug::IEventSink* sink) {
    runtime.bind_debug_sink(sink);
    initialize_runtime(runtime, sequence, profiles, downstream);
  }

  SampleRecord weight(unsigned slot, uint64_t now_us, uint64_t sequence_id) const {
    auto record = organic_attempt(sequence_id, now_us);
    record.header.sensor = hx_sensors[slot];
    record.header.stream = streams[slot];
    record.header.profile_ref = hx[slot].ref();
    record.quality = SampleQuality::Ready;
    record.reason = SampleReason::DataReady;
    record.raw.present = true;
    record.raw.signed_counts = 2000;
    return record;
  }

  void feed(unsigned slot, uint64_t now_us, uint64_t sequence_id) {
    auto record = weight(slot, now_us, sequence_id++);
    runtime.observe(record);
    record.header.sequence = sequence_id++;
    record.header.sensor = us_sensors[slot];
    record.header.kind = SampleKind::UltrasonicEcho;
    record.header.profile_ref = ultrasonic[slot].ref();
    record.raw.distance_us = 6000U;
    runtime.observe(record);
    if (slot != 1U) {
      const unsigned temperature_slot = slot == 0U ? 0U : 1U;
      record.header.sequence = sequence_id++;
      record.header.sensor = slot == 0U ? SensorId::Sht31_44 : SensorId::Sht31_45;
      record.header.kind = SampleKind::Sht31;
      record.header.profile_ref = sht[temperature_slot].ref();
      record.raw.identity_valid = true;
      record.raw.temperature_crc_valid = true;
      record.raw.temperature_centi_c = 2500;
      runtime.observe(record);
    }
    if (slot == 2U) {
      for (unsigned i = 0U; i < 2U; ++i) {
        record.header.sequence = sequence_id++;
        record.header.sensor = i == 0U ? SensorId::Mq7 : SensorId::Mq8;
        record.header.kind = SampleKind::MqAdc;
        record.header.profile_ref = mq[i].ref();
        record.raw.adc_counts = 1000U;
        record.converted.primary_available = true;
        record.converted.primary_kind = ConvertedKind::AdcMillivolts;
        record.converted.primary_value = 1000.0F;
        runtime.observe(record);
      }
    }
  }
};

models::ModelId runtime_model_id(const char* text) {
  models::ModelId id{};
  TEST_ASSERT_TRUE(models::set_model_id(id, text));
  return id;
}

models::FisModelProfile runtime_model(bool trace_overflow) {
  models::FisModelProfile model{};
  model.manifest = profile_manifest(ProfileKind::Fis, ProfileTarget::Unknown);
  model.manifest.target_id = static_cast<uint32_t>(models::ModelTarget::FisOrganic);
  model.manifest.validity = ProfileValidity::TestOnly;
  model.deployment = models::ModelDeployment::TestOnly;
  const auto sets = [](auto& variable) {
    variable.domain_min = 0.0F;
    variable.domain_max = 100.0F;
    variable.set_count = 3U;
    variable.sets[0] = {runtime_model_id("low"), models::MembershipShape::Trapezoid,
                        {0.0F, 0.0F, 30.0F, 50.0F}};
    variable.sets[1] = {runtime_model_id("medium"), models::MembershipShape::Triangle,
                        {30.0F, 50.0F, 70.0F, 70.0F}};
    variable.sets[2] = {runtime_model_id("high"), models::MembershipShape::Trapezoid,
                        {50.0F, 70.0F, 100.0F, 100.0F}};
  };
  model.input_count = trace_overflow ? 3U : 2U;
  model.inputs[0].id = models::ModelInput::FillPercent;
  model.inputs[0].unit = models::ModelUnit::Percent;
  model.inputs[1].id = models::ModelInput::WeightUtilizationPercent;
  model.inputs[1].unit = models::ModelUnit::Percent;
  model.inputs[2].id = models::ModelInput::TemperatureC;
  model.inputs[2].unit = models::ModelUnit::Celsius;
  for (unsigned i = 0U; i < model.input_count; ++i) sets(model.inputs[i]);
  model.output.id = runtime_model_id("collection");
  model.output.unit = models::ModelUnit::Percent;
  sets(model.output);
  model.rule_count = trace_overflow ? 27U : 9U;
  for (unsigned i = 0U; i < model.rule_count; ++i) {
    auto& rule = model.rules[i];
    rule.id = static_cast<uint16_t>(i + 1U);
    rule.antecedent_count = model.input_count;
    rule.antecedent_set_indices[0] = static_cast<uint8_t>(trace_overflow ? i / 9U : i / 3U);
    rule.antecedent_set_indices[1] = static_cast<uint8_t>(trace_overflow ? (i / 3U) % 3U : i % 3U);
    rule.antecedent_set_indices[2] = static_cast<uint8_t>(i % 3U);
    rule.antecedent_operator = models::AntecedentOperator::AndMinimum;
    rule.consequent_set_index = 0U;
  }
  model.centroid_grid_points = 257U;
  model.area_epsilon = 0.000001F;
  model.label_count = 3U;
  model.labels[0] = {runtime_model_id("Normal"), 0.0F};
  model.labels[1] = {runtime_model_id("Waspada"), 40.0F};
  model.labels[2] = {runtime_model_id("Angkat"), 70.0F};
  TEST_ASSERT_TRUE(models::validate_fis_model(model).valid());
  return model;
}

void test_actual_pipeline_model_failure_and_trace_reasons_never_use_default_struct_state() {
  for (unsigned mode = 0U; mode < 6U; ++mode) {
    ReadyCycle fixture{};
    ProcessingSink sink{};
    fixture.begin(&sink);
    fixture.feed(0U, 0U, 100U);
    auto model = runtime_model(mode == 4U);
    if (mode == 1U) model.deployment = models::ModelDeployment::Unconfigured;
    if (mode == 2U) model.rule_count = 0U;
    if (mode == 3U) model.manifest.target_id = static_cast<uint32_t>(models::ModelTarget::FisB3);
    if (mode == 5U) {
      model.area_epsilon = 100000.0F;
      TEST_ASSERT_TRUE(models::validate_fis_model(model).valid());
    }
    if (mode != 0U) fixture.runtime.set_model_profiles(&model, nullptr);
    fixture.runtime.poll(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
    const int32_t expected_state = mode < 2U ? 0 : mode == 3U ? 2 : 1;
    const int32_t expected_reason = mode == 2U ? 7 : mode == 4U ? 8 : 0;
    TEST_ASSERT_EQUAL_INT32(expected_state, sink.events[0].arg0);
    TEST_ASSERT_EQUAL_INT32(expected_reason, sink.events[0].arg1);
    if (mode == 3U) {
      TEST_ASSERT_EQUAL_UINT32(0U, fixture.runtime.handoff().size());
      TEST_ASSERT_TRUE(fixture.runtime.result_failures() > 0U);
    } else {
      ProcessingRuntime::ResultEnvelope result{};
      TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::ResultHandoffStatus::Dequeued),
                            static_cast<int>(fixture.runtime.handoff().try_pop(result)));
      if (mode == 2U) TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::InvalidModel),
          static_cast<int>(result.result.fuzzy.value().reason));
      if (mode == 4U) TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::TraceOverflow),
          static_cast<int>(result.result.fuzzy.value().reason));
      if (mode == 5U) TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisReason::ZeroArea),
          static_cast<int>(result.result.fuzzy.value().reason));
    }
  }
}

void test_aggregate_priority_includes_other_stream_failure_and_preserves_unknown_failure() {
  for (const int32_t priority : {6, 7, 8}) {
    ReadyCycle fixture{};
    ProcessingSink sink{};
    fixture.begin(&sink);
    fixture.feed(0U, 0U, 100U);
    fixture.feed(1U, 0U, 200U);
    auto timeout = fixture.weight(1U, 0U, 400U);
    timeout.quality = SampleQuality::Timeout;
    fixture.runtime.observe(timeout);
    auto out_of_range = fixture.weight(1U, 0U, 401U);
    out_of_range.header.sensor = SensorId::UltrasonicInorganic;
    out_of_range.header.kind = SampleKind::UltrasonicEcho;
    out_of_range.header.profile_ref = fixture.ultrasonic[1].ref();
    out_of_range.quality = SampleQuality::OutOfRange;
    fixture.runtime.observe(out_of_range);
    auto model = runtime_model(priority == 8);
    if (priority == 7) model.rule_count = 0U;
    if (priority != 6) fixture.runtime.set_model_profiles(
        std::array<const models::FisModelProfile*, 3U>{{&model, nullptr, nullptr}}, nullptr);
    fixture.runtime.poll(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
    TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
    TEST_ASSERT_EQUAL_INT32(priority, sink.events[0].arg1);
  }
  ReadyCycle fixture{};
  ProcessingSink sink{};
  fixture.begin(&sink);
  fixture.feed(0U, 0U, 100U);
  fixture.feed(1U, 0U, 200U);
  auto wrong_model = runtime_model(false);
  wrong_model.manifest.target_id = static_cast<uint32_t>(models::ModelTarget::FisB3);
  fixture.runtime.set_model_profiles(
      std::array<const models::FisModelProfile*, 3U>{{&wrong_model, nullptr, nullptr}}, nullptr);
  fixture.runtime.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT32(0, sink.events[0].arg1);
}

void test_safety_invalid_model_is_distinct_from_a_valid_hazard_alert() {
  for (const bool invalid : {false, true}) {
    ReadyCycle fixture{};
    ProcessingSink sink{};
    fixture.begin(&sink);
    fixture.feed(2U, 0U, 100U);
    models::SafetyModelProfile model{};
    model.manifest = profile_manifest(ProfileKind::Safety, ProfileTarget::Unknown);
    model.manifest.target_id = static_cast<uint32_t>(models::ModelTarget::SafetyB3);
    model.manifest.validity = ProfileValidity::TestOnly;
    model.deployment = models::ModelDeployment::TestOnly;
    model.channel_count = invalid ? 0U : 1U;
    model.channels[0] = {models::HazardSignal::TemperatureC, true, 20.0F, 10.0F};
    model.clear_consecutive_cycles = 2U;
    fixture.runtime.set_model_profiles(nullptr, &model);
    fixture.runtime.poll(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
    TEST_ASSERT_EQUAL_INT32(invalid ? 1 : 0, sink.events[0].arg0);
    TEST_ASSERT_EQUAL_INT32(invalid ? 7 : 0, sink.events[0].arg1);
    ProcessingRuntime::ResultEnvelope result{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::ResultHandoffStatus::Dequeued),
                          static_cast<int>(fixture.runtime.handoff().try_pop(result)));
    if (invalid) TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyReason::InvalidModel),
        static_cast<int>(result.result.safety.value().reason));
    else TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Alert),
        static_cast<int>(result.result.safety.value().state));
  }
}

void test_processing_fresh_sink_modes_keep_nonempty_source_and_result_ownership() {
  for (unsigned mode = 0U; mode < 5U; ++mode) {
    ReadyCycle fixture{};
    ProcessingSink recording{};
    recording.accept = mode != 2U;
    ProcessingDebugLock lock{};
    debug::DebugService service{boot_id(), "processing",
        mode == 3U ? debug::Level::Error : debug::Level::Info, lock};
    if (mode == 4U) {
      debug::EventInput event{};
      event.module = debug::Module::Runtime;
      event.event_code = 1U;
      for (unsigned i = 0U; i < 64U; ++i) TEST_ASSERT_TRUE(service.emit(event));
    }
    fixture.begin(mode == 0U ? nullptr : mode < 3U ?
        static_cast<debug::IEventSink*>(&recording) : &service);
    for (unsigned slot = 0U; slot < 3U; ++slot) fixture.feed(slot, 0U, 100U * (slot + 1U));
    const auto source = fixture.weight(0U, 0U, 900U);
    std::array<unsigned char, sizeof(SampleRecord)> bytes{};
    std::memcpy(bytes.data(), &source, bytes.size());
    SampleRing<1U> handoff{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                          static_cast<int>(handoff.try_push(source)));
    fixture.runtime.observe(source);
    const auto downstream = fixture.downstream.count;
    const auto locks_before = lock.locks;
    fixture.runtime.poll(0U);
    const auto emit_locks = lock.locks - locks_before;
    TEST_ASSERT_EQUAL_UINT64(3U, fixture.sequence.next_value());
    TEST_ASSERT_EQUAL_UINT32(3U, fixture.runtime.handoff().size());
    TEST_ASSERT_EQUAL_UINT32(downstream, fixture.downstream.count);
    TEST_ASSERT_EQUAL_MEMORY(bytes.data(), &source, bytes.size());
    SampleRecord retained{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                          static_cast<int>(handoff.try_peek(retained)));
    TEST_ASSERT_EQUAL_UINT64(source.header.sequence, retained.header.sequence);
    if (mode == 1U) TEST_ASSERT_EQUAL_UINT32(1U, recording.count);
    if (mode == 2U) {
      TEST_ASSERT_EQUAL_UINT32(1U, recording.attempts);
      TEST_ASSERT_EQUAL_UINT32(0U, recording.count);
    }
    if (mode >= 3U) TEST_ASSERT_EQUAL_UINT32(1U, emit_locks);
    if (mode == 3U) TEST_ASSERT_EQUAL_UINT64(0U, service.meta(0U).latest);
    if (mode == 4U) {
      TEST_ASSERT_EQUAL_UINT64(65U, service.meta(0U).latest);
      TEST_ASSERT_EQUAL_UINT64(1U, service.meta(0U).overwritten);
      const auto page = service.read({boot_id(), 64U});
      TEST_ASSERT_TRUE(page.has_event);
      TEST_ASSERT_EQUAL_UINT16(42U, page.event.event_code);
      TEST_ASSERT_EQUAL_INT32(0, page.event.arg0);
    }
    const auto cached_locks = lock.locks;
    fixture.runtime.poll(1000000U);
    TEST_ASSERT_EQUAL_UINT32(cached_locks, lock.locks);
    TEST_ASSERT_EQUAL_UINT64(6U, fixture.sequence.next_value());
    TEST_ASSERT_EQUAL_UINT64(3U, fixture.runtime.handoff().dropped_results());
    TEST_ASSERT_EQUAL_UINT32(1U, handoff.size());
    TEST_ASSERT_EQUAL_UINT64(0U, handoff.dropped_records());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                          static_cast<int>(handoff.try_peek(retained)));
    TEST_ASSERT_EQUAL_MEMORY(bytes.data(), &retained, bytes.size());
    TEST_ASSERT_EQUAL_MEMORY(bytes.data(), &source, bytes.size());
    if (mode == 1U || mode == 2U) TEST_ASSERT_EQUAL_UINT32(1U, recording.attempts);
  }
}

void test_available_aggregate_excludes_unsupported_metrics_and_no_source_streams() {
  for (const bool all_streams : {false, true}) {
    ReadyCycle fixture{};
    ProcessingSink sink{};
    fixture.begin(&sink);
    if (all_streams) {
      fixture.feed(0U, 0U, 100U);
      fixture.feed(2U, 0U, 300U);
    }
    fixture.feed(1U, 0U, 200U);
    fixture.runtime.poll(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
    TEST_ASSERT_EQUAL_INT32(0, sink.events[0].arg0);
    TEST_ASSERT_EQUAL_INT32(0, sink.events[0].arg1);
    TEST_ASSERT_EQUAL_UINT32(all_streams ? 3U : 1U, fixture.runtime.handoff().size());
    ProcessingRuntime::ResultEnvelope result{};
    while (fixture.runtime.handoff().try_pop(result) ==
           processing::ResultHandoffStatus::Dequeued) {
      const auto& values = result.result.processing.processed;
      TEST_ASSERT_TRUE(values.fill_pct.available);
      TEST_ASSERT_TRUE(values.weight_kg.available);
      TEST_ASSERT_TRUE(values.weight_utilization_pct.available);
      TEST_ASSERT_FALSE(values.ogi.available);
      TEST_ASSERT_FALSE(values.accumulation_hours.available);
      if (result.result.processing.stream != StreamId::Inorganic) {
        TEST_ASSERT_TRUE(values.temperature_c.available);
      }
      if (result.result.processing.stream == StreamId::B3) {
        TEST_ASSERT_TRUE(values.mq7_mv.available && values.mq8_mv.available);
      }
    }
  }
}

void test_availability_tracks_real_quality_reasons_and_freshness_boundary() {
  struct Case { SampleQuality quality; int32_t reason; };
  const Case cases[]{{SampleQuality::WarmingUp, 4}, {SampleQuality::HeaterPhase, 4},
      {SampleQuality::Uncalibrated, 4}, {SampleQuality::Calibrating, 3},
      {SampleQuality::Timeout, 5}, {SampleQuality::Disconnected, 5},
      {SampleQuality::CrcError, 5}, {SampleQuality::OutOfRange, 6},
      {SampleQuality::Stale, 2}};
  for (const auto& item : cases) {
    ReadyCycle fixture{};
    ProcessingSink sink{};
    fixture.begin(&sink);
    fixture.feed(1U, 0U, 100U);
    auto record = fixture.weight(1U, 0U, 200U);
    record.quality = item.quality;
    fixture.runtime.observe(record);
    fixture.runtime.poll(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
    TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
    TEST_ASSERT_EQUAL_INT32(item.reason, sink.events[0].arg1);
  }
  ReadyCycle fixture{};
  ProcessingSink sink{};
  fixture.begin(&sink);
  fixture.feed(1U, 0U, 100U);
  fixture.runtime.poll(5000000U);
  TEST_ASSERT_EQUAL_INT32(0, sink.events[0].arg0);
  fixture.runtime.poll(6000000U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[1].arg1);
}

void test_missing_invalid_profile_capacity_and_conversion_reasons_are_explicit() {
  for (unsigned mode = 0U; mode < 6U; ++mode) {
    ReadyCycle fixture{};
    ProcessingSink sink{};
    fixture.begin(&sink);
    fixture.feed(1U, 0U, 100U);
    if (mode == 0U) fixture.profiles.hx711[1] = nullptr;
    if (mode == 1U) fixture.hx[1].counts_per_kg = 0.0F;
    if (mode == 2U) fixture.profiles.capacity[1] = nullptr;
    if (mode == 3U) fixture.capacity[1].operational_capacity_kg = 0.0F;
    if (mode == 4U || mode == 5U) {
      auto record = fixture.weight(1U, 0U, 200U);
      if (mode == 4U) record.raw.present = false;
      else record.header.profile_ref.hash[0] ^= 1U;
      fixture.runtime.observe(record);
    }
    fixture.runtime.poll(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
    TEST_ASSERT_EQUAL_INT32(1, sink.events[0].arg0);
    TEST_ASSERT_EQUAL_INT32(mode == 0U || mode == 2U ? 1 : 5, sink.events[0].arg1);
  }
}

void test_full_result_delivery_and_factory_exhaustion_keep_computed_availability() {
  ReadyCycle fixture{};
  ProcessingSink sink{};
  fixture.begin(&sink);
  for (unsigned slot = 0U; slot < 3U; ++slot) fixture.feed(slot, 0U, 100U * (slot + 1U));
  fixture.runtime.poll(0U);
  ProcessingRuntime::ResultEnvelope prefix{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::ResultHandoffStatus::Dequeued),
                        static_cast<int>(fixture.runtime.handoff().try_peek(prefix)));
  auto failed = fixture.weight(0U, 1000000U, 900U);
  failed.quality = SampleQuality::Timeout;
  fixture.runtime.observe(failed);
  fixture.runtime.poll(1000000U);
  TEST_ASSERT_EQUAL_UINT32(3U, fixture.runtime.handoff().size());
  TEST_ASSERT_EQUAL_UINT64(3U, fixture.runtime.handoff().dropped_results());
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT32(5, sink.events[1].arg1);
  ProcessingRuntime::ResultEnvelope after{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::ResultHandoffStatus::Dequeued),
                        static_cast<int>(fixture.runtime.handoff().try_peek(after)));
  TEST_ASSERT_EQUAL_UINT64(prefix.header.sequence, after.header.sequence);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F,
      prefix.result.processing.processed.weight_kg.value,
      after.result.processing.processed.weight_kg.value);

  ReadyCycle exhausted{std::numeric_limits<uint64_t>::max() - 1U};
  ProcessingSink exhausted_sink{};
  exhausted.begin(&exhausted_sink);
  for (unsigned slot = 0U; slot < 3U; ++slot) exhausted.feed(slot, 0U, 100U * (slot + 1U));
  exhausted.runtime.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, exhausted_sink.count);
  TEST_ASSERT_EQUAL_INT32(0, exhausted_sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT32(0, exhausted_sink.events[0].arg1);
  TEST_ASSERT_EQUAL_UINT32(1U, exhausted.runtime.handoff().size());
  TEST_ASSERT_EQUAL_UINT64(2U, exhausted.runtime.result_failures());
}

void test_no_source_poll_and_begin_disable_rebind_have_truthful_cache_lifecycle() {
  ReadyCycle fixture{};
  ProcessingSink sink{};
  fixture.begin(&sink);
  auto ambient = fixture.weight(0U, 0U, 100U);
  ambient.header.stream = StreamId::Ambient;
  fixture.runtime.observe(ambient);
  fixture.runtime.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(0U, fixture.runtime.handoff().size());
  fixture.feed(1U, 1000000U, 200U);
  fixture.runtime.poll(1000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.count);
  fixture.runtime.bind_debug_sink(&sink);
  fixture.runtime.poll(2000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.attempts);
  fixture.runtime.disable();
  fixture.runtime.poll(3000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.attempts);
  fixture.begin(&sink);
  fixture.runtime.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.attempts);
  fixture.feed(1U, 1000000U, 400U);
  fixture.runtime.poll(1000000U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
}

void test_runtime_observes_sample_without_consuming_acquisition_handoff() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  SampleRing<2U> acquisition_handoff{};
  const auto record = organic_attempt(11U, 100U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                        static_cast<int>(acquisition_handoff.try_push(record)));

  runtime.observe(record);

  TEST_ASSERT_EQUAL_UINT32(1U, downstream.count);
  TEST_ASSERT_EQUAL_UINT32(1U, acquisition_handoff.size());
  TEST_ASSERT_EQUAL_UINT32(1U, runtime.observed_samples());
}

void test_processing_cadence_is_independent_of_summary_period() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  TEST_ASSERT_EQUAL_UINT64(1'000'000U, runtime.processing_period_us());
}

void test_runtime_retains_latest_attempt_per_channel_with_bounded_budget() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  const std::array<std::pair<SensorId, StreamId>, 13U> channels{{
      {SensorId::Hx711Organic, StreamId::Organic},
      {SensorId::Hx711Inorganic, StreamId::Inorganic},
      {SensorId::Hx711B3, StreamId::B3},
      {SensorId::UltrasonicOrganic, StreamId::Organic},
      {SensorId::UltrasonicInorganic, StreamId::Inorganic},
      {SensorId::UltrasonicB3, StreamId::B3},
      {SensorId::Mq4, StreamId::Organic},
      {SensorId::Mq135, StreamId::Organic},
      {SensorId::Mq7, StreamId::B3},
      {SensorId::Mq8, StreamId::B3},
      {SensorId::Ds18b20B3, StreamId::B3},
      {SensorId::Sht31_44, StreamId::Organic},
      {SensorId::Sht31_45, StreamId::B3},
  }};
  uint64_t sequence_number = 100U;
  for (uint32_t round = 0U; round < 20U; ++round) {
    for (const auto [sensor, stream] : channels) {
      auto record = organic_attempt(sequence_number++,
                                    100'000U + round * 100'000U);
      record.header.sensor = sensor;
      record.header.stream = stream;
      runtime.observe(record);
    }
  }

  TEST_ASSERT_EQUAL_UINT32(13U, runtime.retained_samples());
  TEST_ASSERT_EQUAL_UINT64(0U, runtime.dropped_samples());
  TEST_ASSERT_EQUAL_UINT64(260U, runtime.observed_samples());
}

void test_late_poll_uses_observed_time_not_stale_deadline() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  runtime.observe(organic_attempt(500U, 900'000U));
  runtime.poll(2'500'000U);

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_EQUAL_UINT64(2'500'000U, result.header.monotonic_us);
}

void test_capacity_profile_is_captured_in_runtime_snapshot() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  Hx711Profile hx{};
  hx.manifest = profile_manifest(ProfileKind::Calibration,
                                  ProfileTarget::Hx711Organic);
  hx.counts_per_kg = 500.0F;
  hx.tare_offset_counts = 1000;
  profiles.hx711[0] = &hx;

  processing::SlotCapacityProfile capacity{};
  capacity.present = true;
  capacity.stream = StreamId::Organic;
  capacity.operational_capacity_kg = 4.0F;
  capacity.manifest.kind = ProfileKind::Capacity;
  capacity.manifest.schema_version = 1U;
  capacity.manifest.profile_version = 2U;
  capacity.manifest.target_id = 1U;
  capacity.manifest.validity = ProfileValidity::TestOnly;
  capacity.manifest.hash[0] = 0x42U;
  profiles.capacity[0] = &capacity;

  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);
  auto record = organic_attempt(700U, 900'000U);
  record.header.profile_ref = hx.ref();
  record.quality = SampleQuality::Ready;
  record.reason = SampleReason::DataReady;
  record.raw.present = true;
  record.raw.signed_counts = 2000;
  runtime.observe(record);
  runtime.poll(1'000'000U);

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_TRUE(result.result.processing.processed.weight_utilization_pct.available);
  TEST_ASSERT_FLOAT_WITHIN(
      0.001F, 50.0F,
      result.result.processing.processed.weight_utilization_pct.value);
}

void test_runtime_cycle_emits_unconfigured_result_with_shared_provenance() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  runtime.observe(organic_attempt(21U, 100U));
  runtime.poll(5000000U);

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_EQUAL_UINT64(0U, result.header.sequence);
  TEST_ASSERT_EQUAL_UINT64(7U, result.header.config_ref.config_version);
  TEST_ASSERT_EQUAL_UINT32(1U,
                            result.result.processing.source_sample_ids.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(fuzzy::FisState::Unconfigured),
                        static_cast<int>(result.result.fuzzy.value().state));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(safety::SafetyState::Unconfigured),
                        static_cast<int>(result.result.safety.value().state));
}

void test_runtime_full_result_handoff_is_observable_and_preserves_old_result() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);

  runtime.observe(organic_attempt(31U, 100U));
  runtime.poll(5000000U);
  runtime.observe(organic_attempt(32U, 5000100U));
  runtime.poll(10000000U);
  runtime.observe(organic_attempt(33U, 10000100U));
  runtime.poll(15000000U);
  runtime.observe(organic_attempt(34U, 15000100U));
  runtime.poll(20000000U);

  TEST_ASSERT_EQUAL_UINT32(3U, runtime.handoff().size());
  TEST_ASSERT_EQUAL_UINT64(1U, runtime.handoff().dropped_results());

  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_EQUAL_UINT64(0U, result.header.sequence);
  TEST_ASSERT_EQUAL_UINT64(31U,
                           result.result.processing.source_sample_ids.at(0U)->sequence);
}

SampleRecord sht31_attempt(uint64_t sequence, uint64_t monotonic_us,
                            const Sht31Profile& profile) {
  auto record = organic_attempt(sequence, monotonic_us);
  record.header.sensor = SensorId::Sht31_44;
  record.header.kind = SampleKind::Sht31;
  record.header.profile_ref = profile.ref();
  record.quality = SampleQuality::Ready;
  record.reason = SampleReason::DataReady;
  record.raw.present = true;
  record.raw.identity_valid = true;
  record.raw.temperature_crc_valid = true;
  record.raw.temperature_centi_c = 2500;
  return record;
}

void test_runtime_preserves_slow_channel_until_freshness_expires() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  Sht31Profile sht{};
  sht.manifest = profile_manifest(ProfileKind::Calibration, ProfileTarget::Sht31_44);
  sht.address = 0x44U;
  profiles.sht31[0] = &sht;
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);
  runtime.observe(sht31_attempt(42U, 900'000U, sht));

  for (const uint64_t now_us : {1'000'000ULL, 2'000'000ULL}) {
    runtime.poll(now_us);
    ProcessingRuntime::ResultEnvelope result{};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(processing::ResultHandoffStatus::Dequeued),
        static_cast<int>(runtime.handoff().try_pop(result)));
    TEST_ASSERT_TRUE(result.result.processing.processed.temperature_c.available);
    TEST_ASSERT_FLOAT_WITHIN(0.001F, 25.0F,
                             result.result.processing.processed.temperature_c.value);
    TEST_ASSERT_EQUAL_UINT64(42U,
        result.result.processing.source_sample_ids.at(0U)->sequence);
    TEST_ASSERT_EQUAL_UINT64(now_us, result.header.monotonic_us);
  }

  runtime.poll(6'000'000U);
  ProcessingRuntime::ResultEnvelope stale{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(stale)));
  TEST_ASSERT_FALSE(stale.result.processing.processed.temperature_c.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::InputQuality::Stale),
      static_cast<int>(stale.result.processing.processed.temperature_c.disposition.quality));
  TEST_ASSERT_EQUAL_UINT64(42U,
      stale.result.processing.source_sample_ids.at(0U)->sequence);
  TEST_ASSERT_EQUAL_UINT32(1U, downstream.count);
}

void test_runtime_newest_failed_attempt_supersedes_retained_ready() {
  SequenceAllocator sequence{};
  TargetProfileSet profiles{};
  Sht31Profile sht{};
  sht.manifest = profile_manifest(ProfileKind::Calibration, ProfileTarget::Sht31_44);
  sht.address = 0x44U;
  profiles.sht31[0] = &sht;
  CountingObserver downstream{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime, sequence, profiles, downstream);
  runtime.observe(sht31_attempt(42U, 900'000U, sht));
  runtime.poll(1'000'000U);
  ProcessingRuntime::ResultEnvelope result{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_TRUE(result.result.processing.processed.temperature_c.available);

  auto failed = sht31_attempt(43U, 1'900'000U, sht);
  failed.quality = SampleQuality::Timeout;
  failed.reason = SampleReason::Timeout;
  failed.raw.present = false;
  runtime.observe(failed);
  runtime.poll(2'000'000U);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(processing::ResultHandoffStatus::Dequeued),
      static_cast<int>(runtime.handoff().try_pop(result)));
  TEST_ASSERT_FALSE(result.result.processing.processed.temperature_c.available);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(processing::InputQuality::Timeout),
      static_cast<int>(result.result.processing.processed.temperature_c.disposition.quality));
  TEST_ASSERT_EQUAL_UINT64(43U,
      result.result.processing.source_sample_ids.at(0U)->sequence);
}

void test_processing_resource_footprint_is_bounded() {
  std::printf("memory ProcessingRuntime=%zu CycleWorkspace=%zu "
              "ProcessingSnapshot=%zu ProcessedValues=%zu ResultEnvelope=%zu "
              "ModelLifecycle=%zu\\n",
              sizeof(ProcessingRuntime), sizeof(ProcessingRuntime::CycleWorkspace),
              sizeof(processing::ProcessingSnapshot),
              sizeof(processing::ProcessedValues),
              sizeof(ProcessingRuntime::ResultEnvelope),
              sizeof(models::ModelProfileLifecycle));
  TEST_ASSERT_TRUE(sizeof(ProcessingRuntime::CycleWorkspace) <= 16000U);
  TEST_ASSERT_TRUE(sizeof(ProcessingRuntime) <= 32000U);
  TEST_ASSERT_TRUE(sizeof(ProcessingRuntime::Handoff) < 20000U);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_actual_pipeline_model_failure_and_trace_reasons_never_use_default_struct_state);
  RUN_TEST(test_aggregate_priority_includes_other_stream_failure_and_preserves_unknown_failure);
  RUN_TEST(test_safety_invalid_model_is_distinct_from_a_valid_hazard_alert);
  RUN_TEST(test_processing_fresh_sink_modes_keep_nonempty_source_and_result_ownership);
  RUN_TEST(test_available_aggregate_excludes_unsupported_metrics_and_no_source_streams);
  RUN_TEST(test_availability_tracks_real_quality_reasons_and_freshness_boundary);
  RUN_TEST(test_missing_invalid_profile_capacity_and_conversion_reasons_are_explicit);
  RUN_TEST(test_full_result_delivery_and_factory_exhaustion_keep_computed_availability);
  RUN_TEST(test_no_source_poll_and_begin_disable_rebind_have_truthful_cache_lifecycle);
  RUN_TEST(test_availability_uses_real_cycle_reason_and_keeps_acquisition_record);
  RUN_TEST(test_runtime_observes_sample_without_consuming_acquisition_handoff);
  RUN_TEST(test_processing_cadence_is_independent_of_summary_period);
  RUN_TEST(test_runtime_retains_latest_attempt_per_channel_with_bounded_budget);
  RUN_TEST(test_late_poll_uses_observed_time_not_stale_deadline);
  RUN_TEST(test_capacity_profile_is_captured_in_runtime_snapshot);
  RUN_TEST(test_runtime_cycle_emits_unconfigured_result_with_shared_provenance);
  RUN_TEST(test_runtime_full_result_handoff_is_observable_and_preserves_old_result);
  RUN_TEST(test_runtime_preserves_slow_channel_until_freshness_expires);
  RUN_TEST(test_runtime_newest_failed_attempt_supersedes_retained_ready);
  RUN_TEST(test_processing_resource_footprint_is_bounded);
  return UNITY_END();
}
