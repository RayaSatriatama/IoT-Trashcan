// Actual app composition with the existing fake SDK, not physical/HIL evidence.
#include "sigap_app.h"
#include "fixtures.h"
#include <LittleFS.h>
#include <cassert>
#include <cstring>

namespace sigap::board { void log_runtime_board_memory() noexcept {} }
namespace sigap {
struct DebugAppProbe {
  static storage::IArchiveStore& store(SigapApp& a) { return a.littlefs_store_; }
  static acquisition::RecordContext context(SigapApp& a) { return a.record_context_; }
  static bool pending(SigapApp& a) { return a.batch_runtime_.pending(); }
  static const telemetry_archive::ImmutableBatch& batch(SigapApp& a) { return a.batch_runtime_.batch(); }
  static bool lock(SigapApp& a) { return a.io_gate_.try_acquire(debug::IoOwner::DebugHttp); }
  static void unlock(SigapApp& a) { a.io_gate_.release(debug::IoOwner::DebugHttp); }
  static void isolate(SigapApp& a) { a.acquisition_.disable(); a.network_ready_ = false; }
};
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const bool current = std::strcmp(argv[1], "current") == 0;
  const bool foreign = std::strcmp(argv[1], "foreign") == 0;
  const bool zero_boot = std::strcmp(argv[1], "zero_boot") == 0;
  const bool invalid_identity = foreign || zero_boot;
  seed_test_boot(); sigap::SigapApp app; app.setup();
  using namespace sigap;
  DebugAppProbe::isolate(app);
  const auto context = DebugAppProbe::context(app);
  auto file = LittleFS.open("/sigap/archive.bin", FILE_WRITE); file.close();
  auto& store = DebugAppProbe::store(app); assert(store.recover());
  acquisition::SampleRecord s{};
  s.header.device_id = context.device_id; s.header.boot_id = context.boot_id;
  if (!current && !invalid_identity) s.header.boot_id.bytes[0] ^= 0x80U;
  if (foreign) s.header.device_id.bytes[0] ^= 0x80U;
  if (zero_boot) s.header.boot_id.bytes.fill(0U);
  s.header.sequence = 90001U; s.header.monotonic_us = 100U;
  s.header.config_ref = context.config_ref;
  s.header.sensor = invalid_identity ? acquisition::SensorId::Mq4 : acquisition::SensorId::Mq7;
  s.header.stream = invalid_identity ? acquisition::StreamId::Organic : acquisition::StreamId::B3;
  s.header.kind = acquisition::SampleKind::MqAdc;
  s.quality = acquisition::SampleQuality::Ready;
  s.reason = acquisition::SampleReason::DataReady;
  s.raw.present = true; s.raw.adc_counts = 123U;
  storage::ImmutableRecordBytes old{}, eligible{};
  assert(storage::RecordCodec::encode_sample(s, old));
  s.header.boot_id = context.boot_id; s.header.sequence = 90002U;
  s.header.sensor = acquisition::SensorId::Mq4; s.header.stream = acquisition::StreamId::Organic;
  assert(storage::RecordCodec::encode_sample(s, eligible));
  assert(store.append(old) == storage::AppendStatus::Durable);
  assert(store.append(eligible) == storage::AppendStatus::Durable);
  const auto original = test_platform::entries.at("/sigap/archive.bin")->data;
  assert(DebugAppProbe::lock(app));
  test_platform::now_us = 1000U; app.loop();
  assert(!DebugAppProbe::pending(app));
  assert(test_platform::entries.at("/sigap/archive.bin")->data == original);
  DebugAppProbe::unlock(app);
  test_platform::now_us = 2000U; app.loop();
  if (current || invalid_identity) {
    assert(!DebugAppProbe::pending(app));
    assert(static_cast<storage::LegacyPendingArchiveStore&>(store).selection_faulted() == (invalid_identity || current));
  } else {
    assert(DebugAppProbe::pending(app));
    assert(DebugAppProbe::batch(app).source.sequence == eligible.identity.sequence);
  }
  const auto& after = test_platform::entries.at("/sigap/archive.bin")->data;
  assert(after.size() >= original.size());
  assert(std::memcmp(after.data(), original.data(), original.size()) == 0);
}
