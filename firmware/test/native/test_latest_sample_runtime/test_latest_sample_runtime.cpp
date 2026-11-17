#include "latest_sample_runtime.h"
#include "acquisition/target_runtime.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace sigap;
static unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::printf("FAIL line %d: %s\n",__LINE__,#value); return 1; } } while(0)
static acquisition::RecordContext context() {
  acquisition::RecordContext c;
  c.device_id.present=true; c.device_id.bytes.fill(0xA1);
  c.boot_id.present=true; c.boot_id.bytes.fill(0xB2);
  c.config_ref.present=true; c.config_ref.schema_version=1; c.config_ref.config_version=1;
  c.config_ref.hash.fill(0xC3); return c;
}
static acquisition::SampleRecord sample(uint64_t seq, uint64_t time,
                                      acquisition::SensorId sensor=acquisition::SensorId::Hx711Organic) {
  acquisition::SampleRecord s;
  auto c=context(); s.header.device_id=c.device_id; s.header.boot_id=c.boot_id;
  s.header.config_ref=c.config_ref; s.header.sensor=sensor;
  s.header.sequence=seq; s.header.monotonic_us=time; s.header.stream=acquisition::StreamId::Organic;
  s.quality=acquisition::SampleQuality::Ready; s.reason=acquisition::SampleReason::DataReady;
  s.raw.present=true; s.converted.primary_available=true;
  if(sensor==acquisition::SensorId::Mq135) {
    s.header.kind=acquisition::SampleKind::MqAdc; s.raw.adc_counts=2048;
    s.converted.primary_kind=acquisition::ConvertedKind::AdcMillivolts; s.converted.primary_value=1650;
  } else if(sensor==acquisition::SensorId::UltrasonicOrganic) {
    s.header.kind=acquisition::SampleKind::UltrasonicEcho; s.raw.distance_us=1400;
    s.converted.primary_kind=acquisition::ConvertedKind::FillPercent; s.converted.primary_value=0;
  } else if(sensor==acquisition::SensorId::Sht31_44) {
    s.header.kind=acquisition::SampleKind::Sht31; s.raw.sht31_ticks_available=true;
    s.raw.sht31_temperature_ticks=30000; s.raw.sht31_humidity_ticks=40000;
    s.raw.temperature_crc_valid=true; s.raw.humidity_crc_valid=true;
    s.converted.primary_kind=acquisition::ConvertedKind::Celsius; s.converted.primary_value=25;
    s.converted.secondary_available=true; s.converted.secondary_kind=acquisition::ConvertedKind::HumidityPercent;
    s.converted.secondary_value=60;
  } else {
    s.header.kind=acquisition::SampleKind::Hx711Counts; s.raw.signed_counts=1250;
    s.converted.primary_kind=acquisition::ConvertedKind::Kilograms; s.converted.primary_value=1.25F;
  }
  return s;
}
static telemetry_archive::Ack ack_for(const telemetry_archive::ImmutableBatch& b) {
  telemetry_archive::Ack a; a.device_id=context().device_id; a.batch_id=b.batch_id;
  a.batch_id_size=b.batch_id_size; a.body_hash=b.body_hash; a.record_count=b.record_count;
  a.stored_count=b.record_count; a.status=telemetry_archive::AckStatus::Durable; return a;
}
static int common_snapshot_cadence() {
  LatestSampleRuntime r; acquisition::SequenceAllocator seq;
  CHECK(r.begin(context(),seq,0,"cadence-fixture",1));
  CHECK(r.prepare(0));
  CHECK(r.apply_ack(ack_for(r.batch()))==telemetry_archive::AckPersistStatus::Durable);
  constexpr acquisition::SensorId sensors[]{acquisition::SensorId::Hx711Organic,
      acquisition::SensorId::Mq135,acquisition::SensorId::UltrasonicOrganic,acquisition::SensorId::Sht31_44};
  uint64_t sequence=1;
  for(auto sensor:sensors) r.observe(sample(sequence++,100,sensor));
  CHECK(r.prepare(100) && r.batch().record_count==4);
  CHECK(r.apply_ack(ack_for(r.batch()))==telemetry_archive::AckPersistStatus::Durable);
  for(auto sensor:sensors) r.observe(sample(sequence++,1000000,sensor));
  CHECK(!r.prepare(1000099));
  CHECK(r.prepare(1000100) && r.batch().record_count==4);
  CHECK(r.last_start_age_us()==100);
  r.discard_unconfirmed();
  CHECK(!r.prepare(2000100));
  return 0;
}
struct TimedSht final : acquisition::ISht31Reader {
  uint64_t now_us{}, ready_at{}; bool converting{};
  acquisition::Sht31ReadResult read(uint8_t) noexcept override {
    acquisition::Sht31ReadResult result{};
    if(!converting) { converting=true; ready_at=now_us+15000; result.pending=true; return result; }
    if(now_us<ready_at) { result.pending=true; return result; }
    converting=false; result.ok=true; result.crc_valid=true;
    result.temperature_ticks=30000; result.humidity_ticks=40000;
    result.temperature_crc_valid=true; result.humidity_crc_valid=true;
    result.temperature_centi_c=2500; result.humidity_centi_pct=6000; return result;
  }
};
static int asynchronous_graph_snapshot() {
  using namespace acquisition;
  InactiveHx711Reader hx(0,0); InactiveUltrasonicReader us(0,0);
  InactiveMqAdcReader mq(0); InactiveDs18b20Reader ds(0); InactiveHeaterRail heater(0);
  TimedSht sht;
  TargetReaderSet readers{{&hx,&hx,&hx},{&us,&us,&us},{&mq,&mq,&mq,&mq},&ds,{&sht,&sht},&heater};
  TargetProfileSet profiles{}; auto ctx=context(); SequenceAllocator seq; RecordFactory factory(seq,ctx);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> ring;
  TargetAcquisitionGraph graph(factory,readers,profiles,ring);
  config::SamplingConfig sampling{};
  sampling.hx711_period_ms=1000; sampling.mq_period_ms=1000; sampling.ultrasonic_period_ms=1000;
  sampling.ds18b20_period_ms=1000; sampling.sht31_period_ms=2000;
  LatestSampleRuntime delivery;
  CHECK(delivery.begin(ctx,seq,0,"synthetic-graph-cadence",1));
  CHECK(delivery.prepare(0));
  CHECK(delivery.apply_ack(ack_for(delivery.batch()))==telemetry_archive::AckPersistStatus::Durable);
  CHECK(graph.begin(sampling,0) && graph.registered_jobs()==4);
  unsigned sht_records{}, frames{};
  for(uint64_t now=0;now<=5050000;now+=1000) {
    sht.now_us=now; graph.poll(now); SampleRecord record{};
    while(ring.try_pop(record)==HandoffStatus::Dequeued) {
      if(record.header.sensor==SensorId::Sht31_44) ++sht_records;
      delivery.observe(record);
    }
    if(now%1000000!=50000) continue;
    CHECK(delivery.prepare(now) && delivery.batch().record_count==4);
    CHECK(delivery.apply_ack(ack_for(delivery.batch()))==telemetry_archive::AckPersistStatus::Durable);
    ++frames;
  }
  CHECK(frames==6 && sht_records==6 && ring.dropped_records()==0);
  return 0;
}
int main() {
  if(common_snapshot_cadence()!=0) return 1;
  if(asynchronous_graph_snapshot()!=0) return 1;
  LatestSampleRuntime r; acquisition::SequenceAllocator seq;
  CHECK(r.begin(context(),seq,0,"unit-test-latest",1));
  CHECK(r.prepare(1000) && r.pending() && r.batch().record_count==1);
  CHECK(r.batch().exact_body().find("unit-test-latest")!=std::string_view::npos);
  CHECK(r.apply_ack(ack_for(r.batch()))==telemetry_archive::AckPersistStatus::Durable);
  for(uint64_t i=1;i<=500;++i) r.observe(sample(i,i*1000));
  CHECK(r.observed()==500 && r.retained()==1 && !r.pending() && r.superseded()==499);
  CHECK(r.prepare(500000) && r.batch().record_count==1);
  CHECK(r.batch().exact_body().find("\"sequence\":\"500\"")!=std::string_view::npos);
  const std::string frozen(r.batch().exact_body());
  for(uint64_t i=501;i<=1000;++i) r.observe(sample(i,i*1000));
  CHECK(r.prepare(1000000) && r.batch().exact_body()==frozen);
  auto bad=ack_for(r.batch()); ++bad.record_count;
  CHECK(r.apply_ack(bad)==telemetry_archive::AckPersistStatus::Invalid && r.pending());
  CHECK(r.apply_ack(ack_for(r.batch()))==telemetry_archive::AckPersistStatus::Durable);
  CHECK(r.prepare(1500000) && r.batch().exact_body().find("\"sequence\":\"1000\"")!=std::string_view::npos);
  const auto delivered=r.records_delivered(); r.discard_unconfirmed();
  CHECK(!r.pending() && r.discarded_records()==1 && r.records_delivered()==delivered);
  CHECK(!r.prepare(2500000)); // A discarded sample is not replayed after the cadence deadline either.
  r.observe(sample(1001,1001000));
  CHECK(r.prepare(2500001));
  CHECK(r.batch().exact_body().find("\"sequence\":\"1001\"")!=std::string_view::npos);
  CHECK(r.apply_ack(ack_for(r.batch()))==telemetry_archive::AckPersistStatus::Durable);
  r.observe(sample(1002,2502000));
  CHECK(!r.prepare(2502000+LatestSampleRuntime::kMaxStartAgeUs+1) && r.expired()==1);
  CHECK(!r.prepare(2502000+LatestSampleRuntime::kMaxStartAgeUs+2) && r.expired()==1);
  auto invalid=sample(1003,6000000); invalid.quality=acquisition::SampleQuality::Timeout;
  invalid.reason=acquisition::SampleReason::Timeout; invalid.raw={}; invalid.converted={};
  r.observe(sample(1002,5999999)); r.observe(invalid);
  CHECK(r.prepare(6000000));
  CHECK(r.batch().exact_body().find("\"quality\":\"TIMEOUT\"")!=std::string_view::npos);
  CHECK(r.batch().exact_body().find("\"converted\":null")!=std::string_view::npos);
  CHECK(r.apply_ack(ack_for(r.batch()))==telemetry_archive::AckPersistStatus::Durable);
  r.observe(sample(1004,7000000));
  r.observe(sample(1005,7000000,acquisition::SensorId::Mq135));
  r.observe(sample(1006,7000000,acquisition::SensorId::UltrasonicOrganic));
  r.observe(sample(1007,7000000,acquisition::SensorId::Sht31_44));
  CHECK(r.retained()==4 && r.prepare(7000000) && r.batch().record_count==4);
  CHECK(r.batch().exact_body().find("\"fill_percent\":0")!=std::string_view::npos);
  CHECK(r.last_start_age_us()==0);
  const auto rejected_before=r.rejected();
  auto foreign=sample(1008,7000001); foreign.header.boot_id.bytes[0]=0;
  r.observe(foreign); CHECK(r.rejected()==rejected_before+1);
  CHECK(r.apply_ack(ack_for(r.batch()))==telemetry_archive::AckPersistStatus::Durable);
  CHECK(!r.prepare(8000000));
  std::printf("PASS %u checks: latest-only, immutable flight, exact ACK, no replay, age and invalid/zero preservation; synthetic fixture only\n",checks);
  return 0;
}
