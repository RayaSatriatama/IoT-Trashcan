#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unity.h>

#include "../../../src/app/processing_runtime.h"
#include "acquisition/handoff.h"
#include "acquisition/record.h"
#include "debug/codec.h"
#include "debug/registry.h"
#include "debug/service.h"
#include "runtime_debug_observer.h"
#include "sigap/config/config_defaults.h"
#include "../../../src/app/runtime_debug_observer.cpp"

namespace {
using namespace sigap;

class Lock final : public debug::IDebugLock {
 public:
  void lock() noexcept override { ++depth; ++entries; }
  void unlock() noexcept override { --depth; }
  unsigned depth{0U};
  unsigned entries{0U};
};

class Evidence final : public acquisition::ISampleObserver {
 public:
  void observe(const acquisition::SampleRecord& sample) noexcept override {
    ++calls;
    last = &sample;
  }
  unsigned calls{0U};
  const acquisition::SampleRecord* last{nullptr};
};

class Sink final : public debug::IEventSink {
 public:
  explicit Sink(Evidence& evidence) : evidence_(evidence) {}
  bool emit(const debug::EventInput& event) noexcept override {
    forwarded_before_emit = evidence_.calls == 1U && evidence_.last == expected;
    events[count++] = event;
    return accept;
  }
  Evidence& evidence_;
  std::array<debug::EventInput, 64> events{};
  size_t count{0U};
  bool accept{true};
  bool forwarded_before_emit{false};
  const acquisition::SampleRecord* expected{nullptr};
};

acquisition::DeviceId device_id() {
  acquisition::DeviceId id{}; id.present=true; id.bytes[0]=0xD1U; return id;
}
acquisition::BootId boot_id() {
  acquisition::BootId id{}; id.present=true; id.bytes[0]=0xB1U; return id;
}
acquisition::ConfigRef config_ref() {
  acquisition::ConfigRef ref{}; ref.present=true; ref.schema_version=1U;
  ref.config_version=7U; ref.hash[0]=0xA7U; return ref;
}
void initialize_runtime(ProcessingRuntime& runtime,
                        acquisition::SequenceAllocator& sequence,
                        acquisition::TargetProfileSet& profiles,
                        acquisition::ISampleObserver& downstream) {
  auto active=config::kFactoryDefaults;
  processing::ResultRecordContext context{};
  context.device_id=device_id(); context.boot_id=boot_id(); context.config_ref=config_ref();
  runtime.set_downstream_observer(&downstream);
  TEST_ASSERT_TRUE(runtime.begin(active,config_ref(),sequence,context,profiles,0U));
}

acquisition::SampleRecord sample(acquisition::SensorId sensor,
                                 acquisition::SampleQuality quality,
                                 acquisition::SampleReason reason,
                                 uint64_t sequence=0U) {
  acquisition::SampleRecord value{};
  value.header.sensor=sensor;
  value.header.sequence=sequence;
  value.header.monotonic_us=987654U;
  value.header.kind=acquisition::SampleKind::MqAdc;
  value.quality=quality;
  value.reason=reason;
  value.raw.adc_counts=0xBEEF;
  value.converted.primary_available=true;
  value.converted.primary_value=1234.5F;
  return value;
}

void test_forwards_exact_sample_once_before_emission_and_coalesces() {
  Evidence evidence;
  Sink sink(evidence);
  RuntimeDebugObserver observer(sink,evidence);
  auto value=sample(acquisition::SensorId::Mq4,acquisition::SampleQuality::Ready,
                    acquisition::SampleReason::DataReady,0U);
  sink.expected=&value;
  observer.observe(value);
  TEST_ASSERT_EQUAL_UINT32(1U,evidence.calls);
  TEST_ASSERT_EQUAL_PTR(&value,evidence.last);
  TEST_ASSERT_TRUE(sink.forwarded_before_emit);
  TEST_ASSERT_EQUAL_UINT32(1U,sink.count);
  TEST_ASSERT_TRUE(debug::registered(sink.events[0].module,sink.events[0].event_code));
  TEST_ASSERT_EQUAL_INT(0,sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT(1,sink.events[0].arg1);
  TEST_ASSERT_EQUAL_UINT64(987654U,sink.events[0].monotonic_us);
  TEST_ASSERT_EQUAL_UINT64(0U,sink.events[0].related_sequence);
  TEST_ASSERT_EQUAL_UINT8(debug::kHasRelatedSequence,sink.events[0].presence);
  observer.observe(value);
  TEST_ASSERT_EQUAL_UINT32(2U,evidence.calls);
  TEST_ASSERT_EQUAL_UINT32(1U,sink.count);
}

void test_quality_and_reason_changes_reemit_and_distinct_channels_do_not_coalesce() {
  Evidence evidence;
  Sink sink(evidence);
  RuntimeDebugObserver observer(sink,evidence);
  auto v=sample(acquisition::SensorId::Mq4,acquisition::SampleQuality::Ready,
                acquisition::SampleReason::DataReady);
  observer.observe(v);
  v.quality=acquisition::SampleQuality::Timeout;
  v.reason=acquisition::SampleReason::Timeout;
  observer.observe(v);
  observer.observe(v);
  v.reason=acquisition::SampleReason::Disconnected;
  observer.observe(v);
  v=sample(acquisition::SensorId::Mq135,acquisition::SampleQuality::Ready,
           acquisition::SampleReason::DataReady);
  observer.observe(v);
  TEST_ASSERT_EQUAL_UINT32(4U,sink.count);
  TEST_ASSERT_EQUAL_INT(2,sink.events[1].arg0);
  TEST_ASSERT_EQUAL_INT(2,sink.events[2].arg0);
  TEST_ASSERT_EQUAL_INT(1,sink.events[3].arg1);
  TEST_ASSERT_EQUAL_INT(2,sink.events[0].level);
}

void test_all_sensor_families_map_without_enum_ordinal_casts() {
  constexpr std::array<acquisition::SensorId,13> sensors{{
    acquisition::SensorId::Mq4, acquisition::SensorId::Mq135,
    acquisition::SensorId::Mq7, acquisition::SensorId::Mq8,
    acquisition::SensorId::Hx711Organic, acquisition::SensorId::Hx711Inorganic,
    acquisition::SensorId::Hx711B3, acquisition::SensorId::UltrasonicOrganic,
    acquisition::SensorId::UltrasonicInorganic, acquisition::SensorId::UltrasonicB3,
    acquisition::SensorId::Ds18b20B3, acquisition::SensorId::Sht31_44,
    acquisition::SensorId::Sht31_45}};
  constexpr std::array<int,13> families{{1,1,1,1,2,2,2,3,3,3,4,4,4}};
  Evidence evidence;
  Sink sink(evidence);
  RuntimeDebugObserver observer(sink,evidence);
  for (size_t i=0;i<sensors.size();++i) {
    auto v=sample(sensors[i],acquisition::SampleQuality::Uncalibrated,
                  acquisition::SampleReason::InvalidProfile);
    observer.observe(v);
    TEST_ASSERT_EQUAL_INT(families[i],sink.events[i].arg1);
    TEST_ASSERT_EQUAL_INT(2,sink.events[i].arg0);
  }
  TEST_ASSERT_EQUAL_UINT32(sensors.size(),sink.count);
}

void test_real_debug_service_full_ring_filtered_and_rejected_unchanged() {
  Lock lock;
  acquisition::BootId boot{};
  boot.present=true;
  debug::DebugService service(boot,"observer-test",debug::Level::Info,lock);
  Evidence evidence;
  RuntimeDebugObserver observer(service,evidence);
  for (unsigned i=0;i<64U;++i) {
    debug::EventInput filler{};
    filler.module=debug::Module::Runtime; filler.event_code=1;
    filler.arg0=0; filler.monotonic_us=i;
    TEST_ASSERT_TRUE(service.emit(filler));
  }
  auto v=sample(acquisition::SensorId::Mq4,acquisition::SampleQuality::Ready,
                acquisition::SampleReason::DataReady,7U);
  observer.observe(v);
  TEST_ASSERT_EQUAL_UINT32(1U,evidence.calls);
  auto meta=service.meta(999U);
  TEST_ASSERT_EQUAL_UINT64(1U,meta.overwritten);
  observer.observe(v);
  TEST_ASSERT_EQUAL_UINT32(2U,evidence.calls);
  TEST_ASSERT_EQUAL_UINT64(1U,service.meta(999U).overwritten);
  TEST_ASSERT_EQUAL_UINT32(0U,lock.depth);
}

void test_unsupported_sensor_and_quality_forward_without_fabricated_events() {
  Evidence evidence;
  Sink sink(evidence);
  RuntimeDebugObserver observer(sink,evidence);
  auto v=sample(static_cast<acquisition::SensorId>(255),acquisition::SampleQuality::Ready,
                acquisition::SampleReason::DataReady);
  observer.observe(v);
  v=sample(acquisition::SensorId::Mq4,static_cast<acquisition::SampleQuality>(255),
           acquisition::SampleReason::DataReady);
  observer.observe(v);
  TEST_ASSERT_EQUAL_UINT32(2U,evidence.calls);
  TEST_ASSERT_EQUAL_UINT32(0U,sink.count);
}

void test_real_service_filters_info_without_blocking_evidence_forwarding() {
  Lock lock;
  acquisition::BootId boot{};
  boot.present=true;
  debug::DebugService service(boot,"observer-filter",debug::Level::Error,lock);
  Evidence evidence;
  RuntimeDebugObserver observer(service,evidence);
  auto v=sample(acquisition::SensorId::Mq4,acquisition::SampleQuality::Ready,
                acquisition::SampleReason::DataReady,17U);
  observer.observe(v);
  TEST_ASSERT_EQUAL_UINT32(1U,evidence.calls);
  TEST_ASSERT_EQUAL_PTR(&v,evidence.last);
  TEST_ASSERT_EQUAL_UINT32(0U,service.meta(v.header.monotonic_us).latest);
}

void test_sink_rejection_does_not_retry_unchanged_state_and_instances_are_independent() {
  Evidence evidence;
  Sink sink(evidence);
  RuntimeDebugObserver first(sink,evidence), second(sink,evidence);
  sink.accept=false;
  auto v=sample(acquisition::SensorId::Mq4,acquisition::SampleQuality::Ready,
                acquisition::SampleReason::DataReady);
  first.observe(v); first.observe(v); second.observe(v);
  TEST_ASSERT_EQUAL_UINT32(2U,sink.count);
  TEST_ASSERT_EQUAL_UINT32(3U,evidence.calls);
}

void test_all_known_qualities_and_ready_invalid_ready_recovery() {
  constexpr std::array<acquisition::SampleQuality,10> qualities{{
    acquisition::SampleQuality::Ready,acquisition::SampleQuality::WarmingUp,
    acquisition::SampleQuality::HeaterPhase,acquisition::SampleQuality::Uncalibrated,
    acquisition::SampleQuality::Calibrating,acquisition::SampleQuality::Timeout,
    acquisition::SampleQuality::Disconnected,acquisition::SampleQuality::CrcError,
    acquisition::SampleQuality::Stale,acquisition::SampleQuality::OutOfRange}};
  constexpr std::array<int,10> categories{{0,1,1,2,2,2,2,2,3,2}};
  Evidence evidence; Sink sink(evidence); RuntimeDebugObserver observer(sink,evidence);
  auto v=sample(acquisition::SensorId::Mq4,qualities[0],acquisition::SampleReason::DataReady);
  for(size_t i=0;i<qualities.size();++i) {
    v.quality=qualities[i]; v.reason=static_cast<acquisition::SampleReason>(i);
    observer.observe(v); TEST_ASSERT_EQUAL_INT(categories[i],sink.events[i].arg0);
  }
  v.quality=acquisition::SampleQuality::Ready; v.reason=acquisition::SampleReason::DataReady;
  observer.observe(v); TEST_ASSERT_EQUAL_UINT32(qualities.size()+1U,sink.count);
  TEST_ASSERT_EQUAL_INT(0,sink.events[sink.count-1U].arg0);
  v.reason=static_cast<acquisition::SampleReason>(255); observer.observe(v);
  TEST_ASSERT_EQUAL_UINT32(qualities.size()+1U,sink.count);
}

struct OwnedSampleFixture {
  acquisition::SequenceAllocator sequence{41U};
  acquisition::SampleRecord record{};
  acquisition::SampleRing<2U> handoff{};
  acquisition::SampleRecord queued_before{};
  std::array<unsigned char,sizeof(acquisition::SampleRecord)> source_bytes{};
  uint64_t next_sequence{0U};

  void prepare(bool mismatch=false) {
    acquisition::RecordContext context{};
    context.device_id=device_id(); context.boot_id=boot_id();
    context.config_ref=config_ref();
    acquisition::RecordFactory factory(sequence,context);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(acquisition::DriverPollStatus::Produced),
      static_cast<int>(factory.make(100U,acquisition::SensorId::Hx711Organic,
        acquisition::StreamId::Organic,acquisition::SampleKind::Hx711Counts,
        acquisition::SampleQuality::Uncalibrated,acquisition::SampleReason::ProfileMissing,
        acquisition::RawObservation{},acquisition::ConvertedObservation{},
        acquisition::ProfileRef{},record)));
    if (mismatch) ++record.header.config_ref.config_version;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(acquisition::HandoffStatus::Enqueued),
                          static_cast<int>(handoff.try_push(record)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(acquisition::HandoffStatus::Dequeued),
                          static_cast<int>(handoff.try_peek(queued_before)));
    next_sequence=sequence.next_value();
    std::memcpy(source_bytes.data(),&record,sizeof(record));
  }

  void assert_unchanged() const {
    TEST_ASSERT_EQUAL_UINT64(next_sequence,sequence.next_value());
    TEST_ASSERT_EQUAL_UINT32(1U,handoff.size());
    TEST_ASSERT_EQUAL_UINT64(0U,handoff.dropped_records());
    acquisition::SampleRecord queued_after{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(acquisition::HandoffStatus::Dequeued),
                          static_cast<int>(handoff.try_peek(queued_after)));
    TEST_ASSERT_EQUAL_MEMORY(&queued_before,&queued_after,sizeof(queued_before));
    TEST_ASSERT_EQUAL_MEMORY(source_bytes.data(),&record,sizeof(record));
  }
};

void test_factory_ring_and_observer_preserve_evidence_path() {
  OwnedSampleFixture input;
  input.prepare();
  Evidence evidence; Sink sink(evidence); RuntimeDebugObserver observer(sink,evidence);
  observer.observe(input.record);
  TEST_ASSERT_EQUAL_UINT32(1U,evidence.calls);
  TEST_ASSERT_EQUAL_PTR(&input.record,evidence.last);
  input.assert_unchanged();
}

void test_disabled_runtime_reaches_refusing_sink_and_preserves_ownership() {
  OwnedSampleFixture input;
  input.prepare();
  Evidence original;
  Sink sink(original);
  sink.accept=false; sink.expected=&input.record;
  RuntimeDebugObserver observer(sink,original);
  ProcessingRuntime runtime{};
  runtime.set_downstream_observer(&observer);
  TEST_ASSERT_FALSE(runtime.enabled());
  runtime.observe(input.record);
  TEST_ASSERT_EQUAL_UINT32(1U,sink.count);
  TEST_ASSERT_TRUE(sink.forwarded_before_emit);
  TEST_ASSERT_EQUAL_UINT32(1U,original.calls);
  TEST_ASSERT_EQUAL_PTR(&input.record,original.last);
  input.assert_unchanged();
  runtime.observe(input.record);
  TEST_ASSERT_EQUAL_UINT32(1U,sink.count);
  TEST_ASSERT_EQUAL_UINT32(2U,original.calls);
  TEST_ASSERT_EQUAL_PTR(&input.record,original.last);
  input.assert_unchanged();
}

void test_mismatched_runtime_reaches_real_filter_and_preserves_ownership() {
  OwnedSampleFixture input;
  input.prepare(true);
  Lock lock;
  debug::DebugService service(boot_id(),"i1-filter",debug::Level::Error,lock);
  Evidence original;
  RuntimeDebugObserver observer(service,original);
  acquisition::TargetProfileSet profiles{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime,input.sequence,profiles,observer);
  const auto entries_before=lock.entries;
  runtime.observe(input.record);
  // A filtered emit still locks the real service; a coalesced input never does.
  TEST_ASSERT_EQUAL_UINT32(entries_before+1U,lock.entries);
  TEST_ASSERT_EQUAL_UINT64(0U,service.meta(input.record.header.monotonic_us).latest);
  TEST_ASSERT_EQUAL_UINT64(1U,runtime.rejected_samples());
  TEST_ASSERT_EQUAL_UINT64(0U,runtime.observed_samples());
  TEST_ASSERT_EQUAL_UINT32(1U,original.calls);
  TEST_ASSERT_EQUAL_PTR(&input.record,original.last);
  TEST_ASSERT_EQUAL_UINT32(0U,lock.depth);
  input.assert_unchanged();
}

void test_matching_runtime_reaches_real_full_ring_and_preserves_ownership() {
  OwnedSampleFixture input;
  input.prepare();
  Lock lock;
  const auto boot=boot_id();
  debug::DebugService service(boot,"i1-full",debug::Level::Info,lock);
  for (unsigned i=0U;i<64U;++i) {
    debug::EventInput filler{};
    filler.module=debug::Module::Runtime; filler.event_code=1U;
    filler.monotonic_us=i;
    TEST_ASSERT_TRUE(service.emit(filler));
  }
  Evidence original;
  RuntimeDebugObserver observer(service,original);
  acquisition::TargetProfileSet profiles{};
  ProcessingRuntime runtime{};
  initialize_runtime(runtime,input.sequence,profiles,observer);
  const auto before=service.meta(input.record.header.monotonic_us);
  const auto entries_before=lock.entries;
  runtime.observe(input.record);
  TEST_ASSERT_EQUAL_UINT32(entries_before+1U,lock.entries);
  const auto after=service.meta(input.record.header.monotonic_us);
  TEST_ASSERT_EQUAL_UINT64(before.latest+1U,after.latest);
  TEST_ASSERT_EQUAL_UINT64(before.overwritten+1U,after.overwritten);
  TEST_ASSERT_EQUAL_UINT64(1U,runtime.observed_samples());
  TEST_ASSERT_EQUAL_UINT64(0U,runtime.rejected_samples());
  TEST_ASSERT_EQUAL_UINT32(1U,original.calls);
  TEST_ASSERT_EQUAL_PTR(&input.record,original.last);
  input.assert_unchanged();
  debug::Cursor cursor{};
  cursor.boot=boot; cursor.after=before.latest;
  debug::BatchSnapshot batch{};
  TEST_ASSERT_TRUE(service.capture(cursor,8U,input.record.header.monotonic_us,batch));
  TEST_ASSERT_EQUAL_UINT32(1U,batch.count);
  TEST_ASSERT_EQUAL_UINT16(40U,batch.events[0].event_code);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(debug::Module::Acquisition),
                        static_cast<int>(batch.events[0].module));
  TEST_ASSERT_EQUAL_UINT64(input.record.header.sequence,batch.events[0].related_sequence);
  TEST_ASSERT_EQUAL_UINT8(debug::kHasRelatedSequence,batch.events[0].presence);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(debug::Level::Info),
                        static_cast<int>(batch.events[0].level));
  TEST_ASSERT_EQUAL_UINT32(0U,lock.depth);
  input.assert_unchanged();
}

void test_real_capture_codec_present_zero_and_payload_privacy() {
  auto encode=[](uint16_t sentinel,char* output,size_t& length) {
    Lock lock; auto boot=boot_id();
    debug::DebugService service(boot,"codec-test",debug::Level::Info,lock);
    Evidence evidence; RuntimeDebugObserver observer(service,evidence);
    auto v=sample(acquisition::SensorId::Mq4,acquisition::SampleQuality::Ready,
      acquisition::SampleReason::DataReady,0U);
    v.raw.adc_counts=sentinel; v.converted.primary_value=static_cast<float>(sentinel);
    observer.observe(v); debug::Cursor cursor{}; cursor.boot=boot;
    debug::BatchSnapshot batch{};
    TEST_ASSERT_TRUE(service.capture(cursor,8U,v.header.monotonic_us,batch));
    TEST_ASSERT_EQUAL_UINT32(1U,batch.count);
    const auto& event=batch.events[0];
    TEST_ASSERT_EQUAL_INT(static_cast<int>(debug::Module::Acquisition),static_cast<int>(event.module));
    TEST_ASSERT_EQUAL_UINT16(40U,event.event_code); TEST_ASSERT_EQUAL_INT(0,event.arg0);
    TEST_ASSERT_EQUAL_INT(1,event.arg1); TEST_ASSERT_EQUAL_UINT64(0U,event.related_sequence);
    TEST_ASSERT_EQUAL_UINT8(debug::kHasRelatedSequence,event.presence);
    TEST_ASSERT_EQUAL_UINT64(v.header.monotonic_us,event.monotonic_us);
    TEST_ASSERT_TRUE(debug::encode_upload("device-fixture",batch,output,2048U,length));
  };
  char first[2048]{},second[2048]{}; size_t n1=0U,n2=0U;
  encode(0x1111U,first,n1); encode(0xEEEEU,second,n2);
  TEST_ASSERT_EQUAL_UINT32(n1,n2); TEST_ASSERT_EQUAL_MEMORY(first,second,n1);
  TEST_ASSERT_NOT_NULL(std::strstr(first,"\"related_sequence\":\"0\""));
  TEST_ASSERT_NULL(std::strstr(first,"4369")); TEST_ASSERT_NULL(std::strstr(first,"61166"));
  TEST_ASSERT_NULL(std::strstr(first,"1234.5"));
  TEST_ASSERT_TRUE(std::string_view(first,n1).find("device-fixture")!=std::string_view::npos);
}

void test_native_observer_size_is_reported() {
  std::printf("native sizeof(RuntimeDebugObserver)=%zu\n",sizeof(RuntimeDebugObserver));
  TEST_ASSERT_TRUE(sizeof(RuntimeDebugObserver)>0U);
}

void test_loop_gap_policy_validation_and_boundaries() {
  TEST_ASSERT_TRUE(runtime_debug_policy::valid_loop_gap_bounds(1U, 2U));
  TEST_ASSERT_FALSE(runtime_debug_policy::valid_loop_gap_bounds(0U, 2U));
  TEST_ASSERT_FALSE(runtime_debug_policy::valid_loop_gap_bounds(2U, 2U));
  TEST_ASSERT_FALSE(runtime_debug_policy::valid_loop_gap_bounds(3U, 2U));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(runtime_debug_policy::classify_loop_gap(0U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(runtime_debug_policy::classify_loop_gap(1000000U)));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(runtime_debug_policy::classify_loop_gap(1000001U)));
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(runtime_debug_policy::classify_loop_gap(5000000U)));
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(runtime_debug_policy::classify_loop_gap(5000001U)));
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(runtime_debug_policy::classify_loop_gap(UINT64_MAX)));
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_forwards_exact_sample_once_before_emission_and_coalesces);
  RUN_TEST(test_quality_and_reason_changes_reemit_and_distinct_channels_do_not_coalesce);
  RUN_TEST(test_all_sensor_families_map_without_enum_ordinal_casts);
  RUN_TEST(test_real_debug_service_full_ring_filtered_and_rejected_unchanged);
  RUN_TEST(test_unsupported_sensor_and_quality_forward_without_fabricated_events);
  RUN_TEST(test_real_service_filters_info_without_blocking_evidence_forwarding);
  RUN_TEST(test_sink_rejection_does_not_retry_unchanged_state_and_instances_are_independent);
  RUN_TEST(test_all_known_qualities_and_ready_invalid_ready_recovery);
  RUN_TEST(test_factory_ring_and_observer_preserve_evidence_path);
  RUN_TEST(test_disabled_runtime_reaches_refusing_sink_and_preserves_ownership);
  RUN_TEST(test_mismatched_runtime_reaches_real_filter_and_preserves_ownership);
  RUN_TEST(test_matching_runtime_reaches_real_full_ring_and_preserves_ownership);
  RUN_TEST(test_real_capture_codec_present_zero_and_payload_privacy);
  RUN_TEST(test_native_observer_size_is_reported);
  RUN_TEST(test_loop_gap_policy_validation_and_boundaries);
  UNITY_END();
}
}
int main() { setup(); return 0; }
