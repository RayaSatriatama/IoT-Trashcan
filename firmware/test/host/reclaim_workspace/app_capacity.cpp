// Exercise the capacity callback installed by actual SigapApp::setup().
#include "sigap_app.h"
#include "fixtures.h"
#include <LittleFS.h>
#include <esp_littlefs.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace sigap::board { void log_runtime_board_memory() noexcept {} }
namespace sigap {
struct DebugAppProbe {
  static storage::LegacyPendingArchiveStore& store(SigapApp& app) {
    return app.littlefs_store_;
  }
  static acquisition::RecordContext context(SigapApp& app) { return app.record_context_; }
  static void isolate(SigapApp& app) { app.acquisition_.disable(); app.network_ready_ = false; }
};
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario(argv[1]);
  using namespace sigap;
  seed_test_boot();
  test_littlefs::reset();
  SigapApp app;
  app.setup();
  DebugAppProbe::isolate(app);
  auto& store = DebugAppProbe::store(app);
  const auto context = DebugAppProbe::context(app);
  auto file = LittleFS.open("/sigap/archive.bin", FILE_WRITE);
  file.close();
  assert(store.recover());
  acquisition::SampleRecord sample{};
  sample.header.device_id = context.device_id;
  sample.header.boot_id = context.boot_id;
  sample.header.config_ref = context.config_ref;
  sample.header.monotonic_us = 100;
  sample.header.sensor = acquisition::SensorId::Mq4;
  sample.header.stream = acquisition::StreamId::Organic;
  sample.header.kind = acquisition::SampleKind::MqAdc;
  sample.quality = acquisition::SampleQuality::Ready;
  sample.reason = acquisition::SampleReason::DataReady;
  sample.raw.present = true;
  sample.raw.adc_counts = 123;
  storage::ImmutableRecordBytes first{}, second{};
  sample.header.sequence = 90001;
  assert(storage::RecordCodec::encode_sample(sample, first));
  sample.header.sequence = 90002;
  assert(storage::RecordCodec::encode_sample(sample, second));
  assert(store.append(first) == storage::AppendStatus::Durable);
  assert(store.append(second) == storage::AppendStatus::Durable);
  const auto original = test_platform::entries.at("/sigap/archive.bin")->data;
  const uint64_t required = second.size + storage::LittleFsArchiveStore::kMigrationMetadataReserveBytes;
  auto& sdk = test_littlefs::fixture;
  sdk.calls = 0;
  if (scenario == "sdk_error" || scenario == "retry_after_sdk_error") sdk.status = -1;
  else if (scenario == "zero_total") sdk.total = sdk.used = 0;
  else if (scenario == "used_exceeds_total") { sdk.total = 100; sdk.used = 101; }
  else if (scenario == "zero_free") sdk.used = sdk.total;
  else if (scenario == "below_threshold") sdk.used = sdk.total - required + 1;
  else if (scenario == "exact_threshold") sdk.used = sdk.total - required;
  else assert(scenario == "valid_geometry");

  const bool accepted = scenario == "valid_geometry" || scenario == "exact_threshold";
  const auto mutations = test_platform::fs_mutations;
  const bool result = store.reclaim_pending(first.identity, first.hash);
  assert(sdk.calls == 1 && std::strcmp(sdk.observed_label, "spiffs") == 0);
  assert(result == accepted);
  if (!accepted) {
    assert(test_platform::fs_mutations == mutations);
    assert(test_platform::entries.at("/sigap/archive.bin")->data == original);
    assert(store.pending_bytes() == original.size());
    assert(store.contains(first.identity, first.hash));
    assert(store.contains(second.identity, second.hash));
    assert(store.health() == storage::ArchiveHealth::Blocked);
    assert(!LittleFS.exists("/sigap/archive.bin.migrate"));
    assert(!LittleFS.exists("/sigap/archive.bin.migrate.bak"));
  }
  if (scenario == "retry_after_sdk_error") {
    sdk.status = ESP_OK;
    assert(store.reclaim_pending(first.identity, first.hash));
    assert(sdk.calls == 2);
  }
  if (accepted || scenario == "retry_after_sdk_error") {
    assert(!store.contains(first.identity, first.hash));
    assert(store.contains(second.identity, second.hash));
    assert(test_platform::entries.at("/sigap/archive.bin")->data ==
           std::vector<uint8_t>(second.bytes.begin(), second.bytes.begin() + second.size));
    assert(store.pending_bytes() == second.size);
  }
  std::printf("APP_CAPACITY %s PASS calls=%u label=%s\n", argv[1], sdk.calls, sdk.observed_label);
}
