// Actual application wiring, with the existing fake SDK and injected samples.
// This is not physical acquisition, TLS, NVS or HIL evidence.
#include "sigap_app.h"
#include "fixtures.h"
#include <LittleFS.h>
#include <esp_littlefs.h>
#include <cassert>
#include <cstring>
namespace sigap::board { void log_runtime_board_memory() noexcept {} }
namespace sigap {
struct DebugAppProbe {
  static void isolate(SigapApp& app) { app.acquisition_.disable();app.network_ready_=false; }
  static auto context(SigapApp& app) { return app.record_context_; }
  static auto& store(SigapApp& app) { return app.littlefs_store_; }
  static bool pending(SigapApp& app) { return app.batch_runtime_.pending(); }
  static const auto& batch(SigapApp& app) { return app.batch_runtime_.batch(); }
  static auto ack(SigapApp& app) {
    const auto& batch=app.batch_runtime_.batch();
    telemetry_archive::Ack ack{};
    ack.device_id=batch.device_id;ack.batch_id_size=batch.batch_id_size;
    std::memcpy(ack.batch_id.data(),batch.batch_id.data(),batch.batch_id_size);
    ack.body_hash=batch.body_hash;ack.record_count=batch.record_count;
    ack.stored_count=batch.record_count;ack.status=telemetry_archive::AckStatus::Durable;
    assert(app.io_gate_.try_acquire(debug::IoOwner::Filesystem));
    auto result=app.network_source_.acknowledge(ack);
    app.io_gate_.release(debug::IoOwner::Filesystem);
    assert(app.batch_store_.pending_bytes()==0 && app.ack_journal_.empty());
    return result;
  }
};
}
int main() {
  using namespace sigap;
  static_assert(build::kLiveArchiveSplit && build::kPhysicalReferenceOnly && !build::kSensorSimulation);
  assert(std::strcmp(build::kFirmwareBuildId,"sigap-task6-physical4-v2")==0);
  seed_test_boot();
  // A full retained archive must not be read as the current delivery queue.
  std::vector<uint8_t> retained(6U*1024U*1024U,0x5a);
  auto file=LittleFS.open("/sigap/archive.bin",FILE_WRITE);
  assert(file.write(retained.data(),retained.size())==retained.size());file.close();
  test_littlefs::fixture.used=retained.size();
  SigapApp app;app.setup();DebugAppProbe::isolate(app);
  assert(DebugAppProbe::store(app).retained_legacy_bytes()==retained.size());
  // Drain only startup records actually emitted by the real application.
  for(unsigned i=0;i<32;++i) {
    test_platform::now_us+=1000;app.loop();
    if(DebugAppProbe::pending(app)) assert(DebugAppProbe::ack(app)==network::DeliveryStatus::Durable);
  }
  auto context=DebugAppProbe::context(app);
  acquisition::SampleRecord sample{};
  sample.header.device_id=context.device_id;sample.header.boot_id=context.boot_id;
  sample.header.config_ref=context.config_ref;sample.header.sequence=99000;
  sample.header.monotonic_us=test_platform::now_us;
  sample.header.sensor=acquisition::SensorId::Mq135;
  sample.header.stream=acquisition::StreamId::Organic;
  sample.header.kind=acquisition::SampleKind::MqAdc;
  sample.quality=acquisition::SampleQuality::Uncalibrated;
  sample.reason=acquisition::SampleReason::ProfileMissing;
  sample.raw.present=true;sample.raw.adc_counts=0;
  storage::ImmutableRecordBytes record{};
  assert(storage::RecordCodec::encode_sample(sample,record));
  assert(DebugAppProbe::store(app).append(record)==storage::AppendStatus::Durable);
  test_platform::now_us+=1000;app.loop();
  assert(DebugAppProbe::pending(app));
  const auto& batch=DebugAppProbe::batch(app);
  assert(batch.source.sequence==99000 && batch.exact_body().find("UNCALIBRATED")!=std::string_view::npos);
  assert(DebugAppProbe::ack(app)==network::DeliveryStatus::Durable);
  assert(DebugAppProbe::store(app).pending_bytes()==0);
  assert(test_platform::entries.at("/sigap/archive.bin")->data==retained);
  assert(std::strcmp(test_littlefs::fixture.observed_label,"spiffs")==0);
  std::puts("PASS actual application live generation, SDK capacity probe, UNCALIBRATED present-zero and ACK cleanup; original 6 MiB unchanged");
  return 0;
}
