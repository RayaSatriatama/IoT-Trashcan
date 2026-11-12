#include <cstdint>
#include <cstring>

#include <unity.h>

#include "acquisition/target_runtime.h"
#include "acquisition/boot_identity.h"
#include "acquisition/provenance.h"
#include "acquisition/runtime.h"
#include "config/config_manager.h"
#include "config/config_store.h"
#include "debug/service.h"

namespace {

using namespace sigap::acquisition;

struct HxFake final : IHx711Reader {
  unsigned ready_calls{}, read_calls{};
  bool ready() noexcept override { ++ready_calls; return true; }
  Hx711ReadResult read() noexcept override {
    ++read_calls;
    return Hx711ReadResult{true, false, 100, 1, 0, SampleReason::DataReady};
  }
};

struct UltrasonicFake final : IUltrasonicReader {
  uint32_t trigger_count{0};
  unsigned poll_calls{};
  uint64_t last_trigger_us{0};
  bool trigger(uint64_t now_us) noexcept override {
    ++trigger_count;
    last_trigger_us = now_us;
    return true;
  }
  UltrasonicEchoResult poll(uint64_t) noexcept override {
    ++poll_calls;
    return UltrasonicEchoResult{UltrasonicEchoStatus::Ready, 100};
  }
};

struct MqFake final : IMqAdcReader {
  unsigned read_calls{};
  MqAdcReadResult read() noexcept override {
    ++read_calls;
    return MqAdcReadResult{true, false, 123, SampleReason::DataReady};
  }
};

struct DsFake final : IDs18b20Reader {
  bool begin_ok{true};
  uint32_t begin_calls{0U};
  uint32_t request_calls{0U};
  unsigned read_calls{};
  bool begin() noexcept override {
    ++begin_calls;
    return begin_ok;
  }
  bool request_conversion() noexcept override {
    ++request_calls;
    return true;
  }
  Ds18b20ReadResult read_conversion() noexcept override {
    ++read_calls;
    return Ds18b20ReadResult{true, true, true, 2500, true, true};
  }
};

struct ShtFake final : ISht31Reader {
  unsigned read_calls{};
  Sht31ReadResult read(uint8_t) noexcept override {
    ++read_calls;
    return Sht31ReadResult{true, true, 2500, 5000};
  }
};

struct HeaterFake final : IMq7HeaterRail {
  unsigned calls{};
  bool select_high() noexcept override { ++calls; return true; }
  bool select_low() noexcept override { ++calls; return true; }
};

struct CounterSink final : sigap::debug::IEventSink {
  bool accept{true};
  unsigned attempts{};
  std::array<sigap::debug::EventInput, 32U> events{};
  std::size_t count{};
  bool emit(const sigap::debug::EventInput& event) noexcept override {
    ++attempts;
    if (!accept) return false;
    if (count < events.size()) events[count++] = event;
    return true;
  }
};

struct SampleCounter final : ISampleObserver {
  unsigned calls{};
  unsigned ultrasonic_calls{};
  const SampleRecord* last_address{};
  void observe(const SampleRecord& record) noexcept override {
    last_address = &record;
    ++calls;
    if (record.header.kind == SampleKind::UltrasonicEcho) ++ultrasonic_calls;
  }
};

RecordContext graph_context() {
  RecordContext context{};
  context.boot_id.present = true;
  context.boot_id.bytes[0] = 1U;
  context.device_id.present = true;
  context.device_id.bytes[0] = 2U;
  context.config_ref.present = true;
  context.config_ref.schema_version = 1U;
  context.config_ref.config_version = 2U;
  return context;
}

struct CounterGraph final {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ShtFake sht{};
  HeaterFake heater{};
  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                         {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                         {&mq[0], &mq[1], &mq[2], &mq[3]},
                         &ds, {&sht, &sht}, &heater};
  TargetProfileSet profiles{};
  RecordContext context{graph_context()};
  SequenceAllocator sequence{};
  RecordFactory factory{sequence, context};
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph{factory, readers, profiles, handoff};
  SampleCounter observer{};
  sigap::config::SamplingConfig sampling{};

  CounterGraph() {
    sampling.hx711_period_ms = 1000U;
    sampling.mq_period_ms = 1000U;
    sampling.ultrasonic_period_ms = 1000U;
    sampling.ds18b20_period_ms = 1000U;
    sampling.sht31_period_ms = 1000U;
    graph.set_sample_observer(&observer);
  }

  std::array<unsigned, 6U> reader_calls() const {
    std::array<unsigned, 6U> counts{};
    for (unsigned i = 0U; i < 3U; ++i) {
      counts[0] += hx[i].ready_calls + hx[i].read_calls;
      counts[1] += ultrasonic[i].trigger_count + ultrasonic[i].poll_calls;
    }
    for (const auto& reader : mq) counts[2] += reader.read_calls;
    counts[3] = ds.begin_calls + ds.request_calls + ds.read_calls;
    counts[4] = sht.read_calls;
    counts[5] = heater.calls;
    return counts;
  }
};

struct GraphDebugLock final : sigap::debug::IDebugLock {
  unsigned locks{};
  void lock() noexcept override { ++locks; }
  void unlock() noexcept override {}
};

void test_counters_nonproduced_clock_regression_and_no_sink_retry() {
  struct Acquirer {
    SampleRecord source{};
    const SampleRecord* last_output{};
    unsigned poll_calls{};
    DriverPollStatus status{DriverPollStatus::NoAttempt};
    DriverPollStatus poll(uint64_t, SampleRecord& out) noexcept {
      ++poll_calls;
      last_output = &out;
      if (status == DriverPollStatus::Produced) out = source;
      return status;
    }
  } acquirer;
  acquirer.source.header.sequence = 42U;
  acquirer.source.raw.present = true;
  acquirer.source.raw.adc_counts = 123U;
  std::array<unsigned char, sizeof(SampleRecord)> source_bytes{};
  std::memcpy(source_bytes.data(), &acquirer.source, source_bytes.size());
  SampleRing<1U> ring{};
  SampleCounter observer{};
  CounterSink sink{};
  AcquisitionDebugCounters counters{};
  counters.bind_sink(&sink);
  AcquisitionJob<Acquirer, 1U> job{&acquirer, &ring, nullptr, &observer, &counters};
  AcquisitionJob<Acquirer, 1U>::run(&job, 0U);
  TEST_ASSERT_EQUAL_UINT32(0U, observer.calls);
  acquirer.status = DriverPollStatus::Produced;
  AcquisitionJob<Acquirer, 1U>::run(&job, 10U);
  AcquisitionJob<Acquirer, 1U>::run(&job, 20U);
  TEST_ASSERT_EQUAL_UINT32(2U, observer.calls);
  TEST_ASSERT_EQUAL_PTR(acquirer.last_output, observer.last_address);
  TEST_ASSERT_EQUAL_UINT64(1U, ring.dropped_records());
  counters.poll(500000U);
  counters.poll(100U);
  counters.poll(1000099U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.attempts);
  counters.poll(1000100U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_INT32(2, sink.events[0].arg1);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[1].arg1);
  counters.poll(2000100U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.attempts);
  sink.accept = false;
  AcquisitionJob<Acquirer, 1U>::run(&job, 2000200U);
  counters.poll(3000100U);
  TEST_ASSERT_EQUAL_UINT32(4U, sink.attempts);
  sink.accept = true;
  counters.poll(4000100U);
  TEST_ASSERT_EQUAL_UINT32(4U, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_UINT32(1U, ring.size());
  TEST_ASSERT_EQUAL_UINT32(4U, acquirer.poll_calls);
  TEST_ASSERT_EQUAL_UINT32(3U, observer.calls);
  TEST_ASSERT_EQUAL_MEMORY(source_bytes.data(), &acquirer.source, source_bytes.size());
  SampleRecord retained{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                        static_cast<int>(ring.try_peek(retained)));
  TEST_ASSERT_EQUAL_UINT64(42U, retained.header.sequence);
  counters.reset(5000000U);
  counters.poll(6000000U);
  TEST_ASSERT_EQUAL_UINT32(4U, sink.attempts);
  counters.bind_sink(nullptr);
  AcquisitionJob<Acquirer, 1U>::run(&job, 6000010U);
  counters.poll(7000000U);
  counters.bind_sink(&sink);
  counters.poll(8000000U);
  TEST_ASSERT_EQUAL_UINT32(4U, sink.attempts);
  TEST_ASSERT_EQUAL_UINT32(5U, acquirer.poll_calls);
}

void test_real_graph_sink_modes_preserve_samples_and_full_ultrasonic_handoff() {
  uint64_t expected_sequence{}, expected_dropped{};
  unsigned expected_observed{};
  std::array<unsigned, 6U> expected_readers{};
  for (unsigned mode = 0U; mode < 5U; ++mode) {
    CounterGraph fixture{};
    CounterSink recording{};
    recording.accept = mode != 2U;
    GraphDebugLock lock{};
    sigap::debug::DebugService service{fixture.context.boot_id, "graph",
        mode == 3U ? sigap::debug::Level::Info : sigap::debug::Level::Debug, lock};
    if (mode == 4U) {
      sigap::debug::EventInput event{};
      event.module = sigap::debug::Module::Runtime;
      event.event_code = 1U;
      for (unsigned i = 0U; i < 64U; ++i) TEST_ASSERT_TRUE(service.emit(event));
    }
    fixture.graph.bind_debug_sink(mode == 0U ? nullptr : mode < 3U ?
        static_cast<sigap::debug::IEventSink*>(&recording) : &service);
    TEST_ASSERT_TRUE(fixture.graph.begin(fixture.sampling, 0U));
    fixture.graph.poll(0U);
    SampleRecord prefix{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                          static_cast<int>(fixture.handoff.try_peek(prefix)));
    while (fixture.handoff.size() < TargetAcquisitionGraph::kHandoffCapacity) {
      TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Enqueued),
                            static_cast<int>(fixture.handoff.try_push(prefix)));
    }
    const auto dropped_before = fixture.handoff.dropped_records();
    const auto ultrasonic_before = fixture.observer.ultrasonic_calls;
    fixture.graph.poll(4000U);
    TEST_ASSERT_TRUE(fixture.observer.ultrasonic_calls > 0U);
    TEST_ASSERT_TRUE(fixture.handoff.dropped_records() > dropped_before);
    TEST_ASSERT_EQUAL_UINT64(fixture.observer.ultrasonic_calls - ultrasonic_before,
        fixture.handoff.dropped_records() - dropped_before);
    const auto locks_before = lock.locks;
    fixture.graph.poll(1000000U);
    const auto emit_locks = lock.locks - locks_before;
    SampleRecord retained{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                          static_cast<int>(fixture.handoff.try_peek(retained)));
    TEST_ASSERT_EQUAL_MEMORY(&prefix, &retained, sizeof(prefix));
    TEST_ASSERT_EQUAL_UINT32(TargetAcquisitionGraph::kHandoffCapacity,
                             fixture.handoff.size());
    if (mode == 0U) {
      expected_sequence = fixture.sequence.next_value();
      expected_observed = fixture.observer.calls;
      expected_dropped = fixture.handoff.dropped_records();
      expected_readers = fixture.reader_calls();
    } else {
      TEST_ASSERT_EQUAL_UINT64(expected_sequence, fixture.sequence.next_value());
      TEST_ASSERT_EQUAL_UINT32(expected_observed, fixture.observer.calls);
      TEST_ASSERT_EQUAL_UINT64(expected_dropped, fixture.handoff.dropped_records());
      const auto readers = fixture.reader_calls();
      TEST_ASSERT_EQUAL_MEMORY(expected_readers.data(), readers.data(), sizeof(readers));
    }
    if (mode == 1U) {
      TEST_ASSERT_EQUAL_UINT32(2U, recording.count);
      TEST_ASSERT_EQUAL_INT32(fixture.observer.calls, recording.events[0].arg1);
      TEST_ASSERT_EQUAL_INT32(fixture.handoff.dropped_records(), recording.events[1].arg1);
    }
    if (mode == 2U) {
      TEST_ASSERT_EQUAL_UINT32(2U, recording.attempts);
      TEST_ASSERT_EQUAL_UINT32(0U, recording.count);
    }
    if (mode >= 3U) TEST_ASSERT_EQUAL_UINT32(2U, emit_locks);
    if (mode == 3U) TEST_ASSERT_EQUAL_UINT64(0U, service.meta(1000000U).latest);
    if (mode == 4U) {
      TEST_ASSERT_EQUAL_UINT64(66U, service.meta(1000000U).latest);
      TEST_ASSERT_EQUAL_UINT64(2U, service.meta(1000000U).overwritten);
      const auto page = service.read({fixture.context.boot_id, 64U});
      TEST_ASSERT_TRUE(page.has_event);
      TEST_ASSERT_EQUAL_UINT16(41U, page.event.event_code);
      TEST_ASSERT_EQUAL_INT32(expected_observed, page.event.arg1);
    }
  }
}

void test_graph_disable_begin_and_identical_bind_reset_only_the_window() {
  CounterGraph fixture{};
  CounterSink sink{};
  fixture.graph.bind_debug_sink(&sink);
  TEST_ASSERT_TRUE(fixture.graph.begin(fixture.sampling, 0U));
  fixture.graph.poll(0U);
  TEST_ASSERT_TRUE(fixture.observer.calls > 0U);
  fixture.graph.disable();
  const auto previous = fixture.observer.calls;
  TEST_ASSERT_TRUE(fixture.graph.begin(fixture.sampling, 2000000U));
  fixture.graph.poll(2000000U);
  fixture.graph.bind_debug_sink(&sink);
  fixture.graph.poll(2999999U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.count);
  fixture.graph.poll(3000000U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_INT32(fixture.observer.calls - previous, sink.events[0].arg1);
  const auto sequence = fixture.sequence.next_value();
  fixture.graph.disable();
  fixture.graph.poll(4000000U);
  TEST_ASSERT_EQUAL_UINT64(sequence, fixture.sequence.next_value());
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
}

void test_counters_measure_real_scheduler_and_ultrasonic_full_handoff() {
  CounterGraph fixture{};
  CounterSink sink{};
  fixture.graph.bind_debug_sink(&sink);
  TEST_ASSERT_TRUE(fixture.graph.begin(fixture.sampling, 0U));
  fixture.graph.poll(0U);
  TEST_ASSERT_TRUE(fixture.handoff.size() > 0U);
  SampleRecord prefix{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                        static_cast<int>(fixture.handoff.try_peek(prefix)));
  fixture.graph.poll(4000U);
  TEST_ASSERT_TRUE(fixture.observer.ultrasonic_calls > 0U);
  fixture.graph.poll(999999U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.count);
  const auto sequence_before = fixture.sequence.next_value();
  const auto produced_before = fixture.observer.calls;
  fixture.graph.poll(1000000U);
  TEST_ASSERT_EQUAL_UINT64(fixture.observer.calls - produced_before,
      fixture.sequence.next_value() - sequence_before);
  TEST_ASSERT_TRUE(fixture.handoff.dropped_records() > 0U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
  TEST_ASSERT_EQUAL_UINT16(41U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_INT32(0, sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT32(fixture.observer.calls, sink.events[0].arg1);
  TEST_ASSERT_EQUAL_INT32(1, sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT32(fixture.handoff.dropped_records(), sink.events[1].arg1);
  TEST_ASSERT_EQUAL_UINT64(1000000U, sink.events[0].monotonic_us);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::debug::Level::Debug),
                        static_cast<int>(sink.events[0].level));
  TEST_ASSERT_EQUAL_UINT8(0U, sink.events[0].presence);
  SampleRecord after{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HandoffStatus::Dequeued),
                        static_cast<int>(fixture.handoff.try_peek(after)));
  TEST_ASSERT_EQUAL_MEMORY(&prefix, &after, sizeof(prefix));
  fixture.graph.poll(1000001U);
  TEST_ASSERT_EQUAL_UINT32(2U, sink.count);
}

void test_target_graph_registers_all_jobs_and_shares_handoff() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ShtFake sht{};
  HeaterFake heater{};

  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  TargetProfileSet profiles{};
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1;
  config_ref.config_version = 2;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);

  sigap::config::SamplingConfig sampling{};
  sampling.hx711_period_ms = 1;
  sampling.mq_period_ms = 1;
  sampling.ultrasonic_period_ms = 1;
  sampling.ds18b20_period_ms = 1;
  sampling.sht31_period_ms = 1;
  TEST_ASSERT_TRUE(graph.begin(sampling, 0));
  TEST_ASSERT_EQUAL_UINT32(TargetAcquisitionGraph::kJobCount, graph.registered_jobs());

  graph.poll(0);
  TEST_ASSERT_TRUE(graph.enabled());
  TEST_ASSERT_EQUAL_UINT32(10, graph.handoff().size());
  TEST_ASSERT_EQUAL_UINT32(0U, ds.begin_calls);
  TEST_ASSERT_EQUAL_UINT32(0U, ds.request_calls);

  uint64_t previous = 0;
  bool first = true;
  bool saw_hx = false;
  bool saw_mq = false;
  bool saw_sht = false;
  bool saw_sht44_organic = false;
  bool saw_sht45_b3 = false;
  bool saw_ds_profile_missing = false;
  SampleRecord record{};
  while (graph.handoff().try_pop(record) == HandoffStatus::Dequeued) {
    if (!first) TEST_ASSERT_TRUE(record.header.sequence > previous);
    first = false;
    previous = record.header.sequence;
    TEST_ASSERT_TRUE(record.header.config_ref.present);
    TEST_ASSERT_EQUAL_UINT64(2, record.header.config_ref.config_version);
    saw_hx = saw_hx || record.header.sensor == SensorId::Hx711Organic ||
             record.header.sensor == SensorId::Hx711Inorganic ||
             record.header.sensor == SensorId::Hx711B3;
    saw_mq = saw_mq || record.header.kind == SampleKind::MqAdc;
    saw_sht = saw_sht || record.header.kind == SampleKind::Sht31;
    if (record.header.sensor == SensorId::Sht31_44) {
      saw_sht44_organic = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(StreamId::Organic),
                            static_cast<int>(record.header.stream));
    }
    if (record.header.sensor == SensorId::Sht31_45) {
      saw_sht45_b3 = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(StreamId::B3),
                            static_cast<int>(record.header.stream));
    }
    if (record.header.kind == SampleKind::Ds18b20) {
      saw_ds_profile_missing = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                            static_cast<int>(record.quality));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::ProfileMissing),
                            static_cast<int>(record.reason));
      TEST_ASSERT_FALSE(record.converted.primary_available);
    }
  }
  TEST_ASSERT_TRUE(saw_hx);
  TEST_ASSERT_TRUE(saw_mq);
  TEST_ASSERT_TRUE(saw_sht);
  TEST_ASSERT_TRUE(saw_sht44_organic);
  TEST_ASSERT_TRUE(saw_sht45_b3);
  TEST_ASSERT_TRUE(saw_ds_profile_missing);
}

void test_target_graph_rejects_missing_reader_dependencies_safely() {
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 1U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  TargetReaderSet readers{};
  TargetProfileSet profiles{};
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};

  TEST_ASSERT_FALSE(graph.begin(sampling, 0U));
  TEST_ASSERT_FALSE(graph.enabled());
  TEST_ASSERT_EQUAL_UINT32(0U, graph.registered_jobs());
}

void test_target_graph_keeps_other_channels_alive_when_ds_profile_apply_fails() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ds.begin_ok = false;
  ShtFake sht{};
  HeaterFake heater{};
  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  Ds18b20Profile ds_profile{};
  ds_profile.manifest.validity = ProfileValidity::Valid;
  ds_profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  ds_profile.has_rom = true;
  ds_profile.rom[0] = 0x28U;
  TargetProfileSet profiles{};
  profiles.ds18b20 = &ds_profile;
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 1U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};

  TEST_ASSERT_TRUE(graph.begin(sampling, 0U));
  TEST_ASSERT_TRUE(graph.enabled());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DsChannelState::ProfileApplyFailed),
                        static_cast<int>(graph.ds_channel_state()));
  graph.poll(0U);
  TEST_ASSERT_TRUE(graph.handoff().size() > 0U);
  TEST_ASSERT_EQUAL_UINT32(0U, ds.request_calls);
  SampleRecord record{};
  bool saw_ds_failure = false;
  while (graph.handoff().try_pop(record) == HandoffStatus::Dequeued) {
    if (record.header.kind == SampleKind::Ds18b20) {
      saw_ds_failure = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                            static_cast<int>(record.quality));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::InvalidProfile),
                            static_cast<int>(record.reason));
    }
  }
  TEST_ASSERT_TRUE(saw_ds_failure);
}

void test_target_graph_uses_strict_ds_channel_when_profile_setup_succeeds() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ShtFake sht{};
  HeaterFake heater{};
  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  Ds18b20Profile ds_profile{};
  ds_profile.manifest.validity = ProfileValidity::Valid;
  ds_profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  ds_profile.has_rom = true;
  ds_profile.rom[0] = 0x28U;
  TargetProfileSet profiles{};
  profiles.ds18b20 = &ds_profile;
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 1U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};

  TEST_ASSERT_TRUE(graph.begin(sampling, 0U));
  TEST_ASSERT_EQUAL_UINT32(1U, ds.begin_calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DsChannelState::Ready),
                        static_cast<int>(graph.ds_channel_state()));
  graph.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, ds.request_calls);
}

void test_target_graph_services_ultrasonic_before_one_second_trigger_deadline() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ShtFake sht{};
  HeaterFake heater{};
  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  TargetProfileSet profiles{};
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 4U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};
  sampling.ultrasonic_period_ms = 1000U;
  sampling.hx711_period_ms = 1000U;
  sampling.mq_period_ms = 1000U;
  sampling.ds18b20_period_ms = 1000U;
  sampling.sht31_period_ms = 1000U;

  TEST_ASSERT_TRUE(graph.begin(sampling, 0U));
  graph.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  TEST_ASSERT_EQUAL_UINT32(0U, ultrasonic[1].trigger_count);

  graph.poll(4000U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  TEST_ASSERT_TRUE(graph.handoff().size() > 0U);

  graph.poll(500000U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  graph.poll(999999U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);

  graph.poll(1000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  TEST_ASSERT_TRUE(ultrasonic[1].trigger_count >= 1U);

  // The organic channel retries its due request after the shared gate has
  // served the other pending channels, without changing its one-second
  // sample cadence.
  graph.poll(1060000U);
  TEST_ASSERT_EQUAL_UINT32(2U, ultrasonic[0].trigger_count);
  TEST_ASSERT_TRUE(ultrasonic[0].last_trigger_us - ultrasonic[1].last_trigger_us >=
                  UltrasonicEngineeringDefaults::kInterSensorGapUs);
}

void test_acquisition_requires_active_config_provenance() {
  sigap::config::MemoryConfigStore store{};
  const sigap::config::ValidationContext validation{8ULL * 1024ULL * 1024ULL, 0U};
  sigap::config::ConfigManager bootstrap_manager(store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(bootstrap_manager.begin()));

  BootRuntimeIdentity bootstrap_identity{};
  RecordContext bootstrap_context{bootstrap_identity.boot_id, DeviceId{},
                                  ConfigRef{}, nullptr};
  RecordFactory bootstrap_factory(bootstrap_identity.sequence, bootstrap_context);
  TargetProfileSet profiles{};
  AcquisitionFoundation bootstrap_runtime(bootstrap_manager, bootstrap_factory,
                                           sigap::board::kN16R8Pins, profiles);
  TEST_ASSERT_FALSE(bootstrap_runtime.begin(
      bootstrap_manager.active(), active_config_ref(bootstrap_manager), 0U));
  TEST_ASSERT_FALSE(bootstrap_runtime.enabled());
  ConfigRef forged_ref{};
  forged_ref.present = true;
  TEST_ASSERT_FALSE(bootstrap_runtime.begin(
      bootstrap_manager.active(), forged_ref, 0U));
  TEST_ASSERT_FALSE(bootstrap_runtime.enabled());

  sigap::config::MemoryConfigStore required_store{};
  required_store.set_provisioned(true);
  sigap::config::ConfigManager required_manager(required_store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::ConfigRequired),
      static_cast<int>(required_manager.begin()));
  BootRuntimeIdentity required_identity{};
  RecordContext required_context{required_identity.boot_id, DeviceId{}, ConfigRef{}, nullptr};
  RecordFactory required_factory(required_identity.sequence, required_context);
  AcquisitionFoundation required_runtime(required_manager, required_factory,
                                          sigap::board::kN16R8Pins, profiles);
  TEST_ASSERT_FALSE(required_runtime.begin(
      required_manager.active(), active_config_ref(required_manager), 0U));
  TEST_ASSERT_FALSE(required_runtime.enabled());

  sigap::config::MemoryConfigStore error_store{};
  error_store.fail_next(sigap::config::MemoryConfigStore::Fault::ReadActivation);
  sigap::config::ConfigManager error_manager(error_store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::StorageError),
      static_cast<int>(error_manager.begin()));
  BootRuntimeIdentity error_identity{};
  RecordContext error_context{error_identity.boot_id, DeviceId{}, ConfigRef{}, nullptr};
  RecordFactory error_factory(error_identity.sequence, error_context);
  AcquisitionFoundation error_runtime(error_manager, error_factory,
                                      sigap::board::kN16R8Pins, profiles);
  TEST_ASSERT_FALSE(error_runtime.begin(
      error_manager.active(), active_config_ref(error_manager), 0U));
  TEST_ASSERT_FALSE(error_runtime.enabled());

  TEST_ASSERT_TRUE(bootstrap_manager.start_draft());
  TEST_ASSERT_TRUE(bootstrap_manager.validate_draft_and_enter());
  TEST_ASSERT_TRUE(bootstrap_manager.preview_draft());
  TEST_ASSERT_TRUE(bootstrap_manager.commit_draft());

  sigap::config::ConfigManager active_manager(store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::ActiveLoaded),
      static_cast<int>(active_manager.begin()));
  BootRuntimeIdentity active_identity{};
  RecordContext active_context{active_identity.boot_id, DeviceId{}, ConfigRef{}, nullptr};
  RecordFactory active_factory(active_identity.sequence, active_context);
  AcquisitionFoundation active_runtime(active_manager, active_factory,
                                        sigap::board::kN16R8Pins, profiles);
  const ConfigRef ref = active_config_ref(active_manager);
  TEST_ASSERT_TRUE(ref.present);
  TEST_ASSERT_TRUE(active_runtime.begin(active_manager.active(), ref, 0U));
  TEST_ASSERT_TRUE(active_runtime.enabled());
  TEST_ASSERT_EQUAL_UINT32(14U, active_runtime.registered_jobs());
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_counters_nonproduced_clock_regression_and_no_sink_retry);
  RUN_TEST(test_real_graph_sink_modes_preserve_samples_and_full_ultrasonic_handoff);
  RUN_TEST(test_graph_disable_begin_and_identical_bind_reset_only_the_window);
  RUN_TEST(test_counters_measure_real_scheduler_and_ultrasonic_full_handoff);
  RUN_TEST(test_target_graph_registers_all_jobs_and_shares_handoff);
  RUN_TEST(test_target_graph_rejects_missing_reader_dependencies_safely);
  RUN_TEST(test_target_graph_keeps_other_channels_alive_when_ds_profile_apply_fails);
  RUN_TEST(test_target_graph_uses_strict_ds_channel_when_profile_setup_succeeds);
  RUN_TEST(test_target_graph_services_ultrasonic_before_one_second_trigger_deadline);
  RUN_TEST(test_acquisition_requires_active_config_provenance);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
