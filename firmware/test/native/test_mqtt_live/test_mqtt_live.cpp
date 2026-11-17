#include <cstdio>
#include <string>
#include <limits>
#include "network/mqtt_live.h"
using namespace sigap;
struct Queue final : network::IMqttLiveQueue {
  bool online{false}, admit{true}; unsigned calls{0}; std::string body;
  bool connected() const noexcept override { return online; }
  bool try_enqueue(std::string_view value) noexcept override {
    ++calls; if (!admit) return false; body.assign(value); return true;
  }
};
static acquisition::DeviceId device() { acquisition::DeviceId d; d.present=true; d.bytes[0]=1; return d; }
static acquisition::BootId boot() { acquisition::BootId b; b.present=true; b.bytes[0]=2; return b; }
static acquisition::SampleRecord sample(uint64_t seq) {
  acquisition::SampleRecord s; s.header.device_id=device();s.header.boot_id=boot();
  s.header.sensor=acquisition::SensorId::Hx711Organic;s.header.kind=acquisition::SampleKind::Hx711Counts;
  s.header.sequence=seq;s.header.monotonic_us=seq*1000;s.quality=acquisition::SampleQuality::Ready;
  s.reason=acquisition::SampleReason::DataReady;s.raw.present=true;s.raw.signed_counts=-5;
  s.converted.primary_available=true;s.converted.primary_kind=acquisition::ConvertedKind::Kilograms;
  s.converted.primary_value=0;return s;
}
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x);return 1; } } while(0)
int main() {
  unsigned passed=0;
  CHECK(!network::MqttBrokerConfig{}.valid());
  CHECK(!(network::MqttBrokerConfig{"mqtt://example.com","user","password"}).valid());
  CHECK((network::MqttBrokerConfig{"mqtts://example.com:8883","user","password"}).valid()); ++passed;
  Queue queue; network::MqttLiveStream stream(queue,10000);
  CHECK(stream.begin(device(),boot(),"unit-test",false));
  for(uint64_t i=1;i<=200;++i) { stream.observe(sample(i));stream.poll(i*1000); }
  CHECK(stream.observed()==200 && queue.calls==0 && stream.retained()==1); ++passed;
  queue.online=true;stream.poll(201000);
  CHECK(queue.calls==1 && queue.body.find("\"sequence\":\"200\"")!=std::string::npos);
  CHECK(queue.body.find("\"kg\":0")!=std::string::npos);
  CHECK(queue.body.find("\"counts\":\"-5\"")!=std::string::npos);
  CHECK(queue.body.find("\"recorded_at_ms\":null")!=std::string::npos); ++passed;
  stream.poll(202000);CHECK(queue.calls==1); ++passed;
  auto bad=sample(201);bad.quality=acquisition::SampleQuality::Timeout;bad.converted={};
  stream.observe(bad);stream.poll(212000);
  CHECK(queue.calls==2 && queue.body.find("\"quality\":\"TIMEOUT\"")!=std::string::npos);
  CHECK(queue.body.find("\"converted\":null")!=std::string::npos); ++passed;
  queue.admit=false;stream.observe(sample(202));stream.poll(223000);
  for(uint64_t i=203;i<=220;++i) stream.observe(sample(i));
  CHECK(stream.observed()==220 && stream.enqueue_rejected()==1);
  queue.admit=true;stream.poll(234000);CHECK(queue.body.find("\"sequence\":\"220\"")!=std::string::npos); ++passed;
  auto alien=sample(221);alien.header.boot_id.bytes[0]=3;
  stream.observe(alien);CHECK(stream.rejected()==1); ++passed;
  auto nan=sample(221);nan.converted.primary_value=std::numeric_limits<float>::quiet_NaN();
  stream.observe(nan);stream.poll(245000);
  CHECK(queue.body.find("\"converted\":null")!=std::string::npos); ++passed;
  auto mq=sample(222);mq.header.sensor=acquisition::SensorId::Mq135;mq.header.kind=acquisition::SampleKind::MqAdc;
  mq.raw.adc_counts=1234;mq.converted.primary_kind=acquisition::ConvertedKind::AdcMillivolts;mq.converted.primary_value=1500;
  stream.observe(mq);stream.poll(256000);CHECK(stream.retained()==2);
  CHECK(queue.body.find("mq135")!=std::string::npos && queue.body.find("adc_mv")!=std::string::npos); ++passed;
  CHECK(queue.body.find("password")==std::string::npos && queue.body.find("token")==std::string::npos); ++passed;
  std::printf("PASS %u MQTT scenarios; queue is a unit fake, not broker/device evidence\n",passed);
  std::printf("FIXTURE_JSON %s\n",queue.body.c_str());
}
