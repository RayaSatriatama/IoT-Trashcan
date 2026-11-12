// Host-only actual producer regressions. No live backend, device, or invented ACK.
#include "sigap_app.h"
#include "fixtures.h"
#include "network/https_transport.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace sigap::board { void log_runtime_board_memory() noexcept {} }
namespace sigap {
struct DebugAppProbe {
  static network::IImmutableBatchSource& source(SigapApp& app) { return app.network_source_; }
  static bool pending(SigapApp& app) { return app.batch_runtime_.pending(); }

  static void acquire(SigapApp& app, uint64_t now) { app.acquisition_.poll(now); }
  static bool pop(SigapApp& app, acquisition::SampleRecord& sample) {
    return app.acquisition_.handoff().try_pop(sample) == acquisition::HandoffStatus::Dequeued;
  }
  static const acquisition::Ds18b20Profile& ds(SigapApp& app) { return *app.profiles_.ds18b20; }
  static bool result_head(SigapApp& app) {
    storage::ImmutableRecordBytes record{};
    storage::DecodedResultPayload result{};
    telemetry_archive::ImmutableBatch rebuilt{};
    const telemetry_archive::ImmutableBatch* admitted{nullptr};
    const bool found = app.archive_store_.peek_pending(record);
    const auto decoded = storage::RecordCodec::decode_result(record, result);
    const bool built = telemetry_archive::BatchJsonBuilder{}.build(record, rebuilt);
    const bool queued = app.network_source_.peek_pending(admitted) && admitted != nullptr;
    std::printf("STATE found=%d seq=%llu kind=%u decoded=%u refs=%u sources=%u built=%d queued=%d queued_seq=%llu\n",
                found, static_cast<unsigned long long>(record.identity.sequence),
                static_cast<unsigned>(record.identity.kind), static_cast<unsigned>(decoded),
                result.profile_ref_count, result.source_sample_id_count, built, queued,
                static_cast<unsigned long long>(queued ? admitted->source.sequence : 0U));
    if (!found || record.identity.sequence != 8U || record.identity.kind != storage::ArchiveRecordKind::Result ||
        decoded != storage::TypedDecodeStatus::Decoded || result.profile_ref_count == 0U ||
        result.source_sample_id_count == 0U || !built || !queued || !admitted->source.present ||
        admitted->source.sequence != 8U ||
        admitted->source.record_kind != static_cast<uint8_t>(storage::ArchiveRecordKind::Result) ||
        admitted->exact_body() != rebuilt.exact_body() || !admitted->body_hash_matches()) return false;
    std::cout << "HEAD " << admitted->exact_body() << std::endl;
    return true;
  }
};
}
namespace {
using namespace sigap;
using namespace sigap::acquisition;
int fail(const char* message) { std::fprintf(stderr, "FAIL %s\n", message); return 1; }
struct Clock final : network::IMonotonicClock {
  uint64_t now_us() const noexcept override { return test_platform::now_us; }
};
struct Jitter final : network::IRandomJitter {
  uint32_t sample(uint32_t) noexcept override { return 0U; }
};
struct Link final : network::IWifiLink {
  bool connected() const noexcept override { return true; }
  bool poll_connect() noexcept override { return true; }
};
struct PrefixTransport final : network::IHttpTransport {
  std::size_t delivered{};
  network::TransportResult post_batch(std::string_view body, std::string_view token,
      uint16_t count, uint32_t, network::NetworkHttpResponse& response) noexcept override {
    if (delivered >= 9U || token != "TEST-DEVICE-TOKEN" || count != 1U)
      return network::TransportResult::Failed;
    std::cout << "BATCH " << body << std::endl;
    std::string ack;
    if (!std::getline(std::cin, ack)) return network::TransportResult::Failed;
    response.status_code = 200;
    response.ack_present = network::decode_archive_ack(ack, response.ack);
    if (!response.ack_present) return network::TransportResult::Failed;
    ++delivered;
    return network::TransportResult::Response;
  }
};
int head(SigapApp& app) {
  Clock clock;
  Jitter jitter;
  Link link;
  PrefixTransport transport;
  network::NetworkWorker worker({}, clock, jitter, link, transport, DebugAppProbe::source(app));
  if (!worker.begin("TEST-DEVICE-TOKEN")) return fail("prefix worker begin");
  for (uint64_t step = 0U; step <= 1000U; ++step) {
    test_platform::now_us = 1000U + step * 1000U;
    app.loop();
    if (transport.delivered < 9U && DebugAppProbe::pending(app) &&
        worker.poll() != network::NetworkOutcome::Delivered)
      return fail("retained real ACK failed actual context/journal/reclaim");
    // Keep normal acquisition active: disabling it retires processing state.
    if (transport.delivered == 9U && step == 1000U)
      return DebugAppProbe::result_head(app) ? 0 : fail("first RESULT sequence8 not decodable/buildable/admitted");
  }
  return fail("nine-request prefix was not consumed");
}
struct IdentityFixture {
  SequenceAllocator sequence{};
  DeviceId device{};
  BootId boot{};
  ConfigRef config{};
  IdentityFixture() {
    device.present = boot.present = config.present = true;
    device.bytes[0] = 0xa1U; boot.bytes[0] = 0xb2U;
    config.schema_version = 1U; config.config_version = 4U; config.hash[0] = 0x44U;
  }
};
int profiles() {
  IdentityFixture ids;
  ProcessingRuntime::ResultEnvelope value{};
  value.header.device_id = ids.device; value.header.boot_id = ids.boot;
  value.header.sequence = 20U; value.header.config_ref = ids.config;
  auto& processing = value.result.processing;
  processing.stream = StreamId::Organic;
  processing.computed_monotonic_us = 200U; processing.config_ref = ids.config;
  processing::SourceSampleId source{};
  source.device_id = ids.device; source.boot_id = ids.boot; source.sequence = 10U;
  if (processing.source_sample_ids.try_add(source) != processing::CollectionStatus::Added)
    return fail("source fixture admission");
  for (uint32_t i = 1U; i <= 2U; ++i) {
    ProfileRef ref{};
    ref.present = true; ref.kind = ProfileKind::Calibration; ref.schema_version = i;
    ref.profile_version = i + 2U; ref.target_id = i;
    ref.hash.fill(static_cast<uint8_t>(0x40U + i));
    if (processing.profile_refs.try_add(ref) != processing::CollectionStatus::Added)
      return fail("profile fixture admission");
  }
  storage::MemoryArchiveStore<4U> store(65536U);
  ArchiveRuntime runtime(&store);
  ProcessingRuntime::Handoff handoff{};
  if (handoff.try_push(value) != processing::ResultHandoffStatus::Enqueued ||
      !runtime.drain_results(handoff) || runtime.poll_storage() != storage::AppendStatus::Durable)
    return fail("actual result serializer/store");
  storage::ImmutableRecordBytes record{};
  storage::DecodedResultPayload decoded{};
  if (!store.peek_pending(record) ||
      storage::RecordCodec::decode_result(record, decoded) != storage::TypedDecodeStatus::Decoded ||
      decoded.profile_ref_count != 2U || decoded.source_sample_id_count != 1U ||
      decoded.source_sample_ids[0].sequence != 10U ||
      decoded.source_sample_ids[0].device_id.bytes != ids.device.bytes ||
      decoded.source_sample_ids[0].boot_id.bytes != ids.boot.bytes)
    return fail("actual RESULT serializer loses profile/source fields");
  for (std::size_t i = 0U; i < 2U; ++i) {
    const auto& expected = *processing.profile_refs.at(i);
    const auto& actual = decoded.profile_refs[i];
    if (!actual.present || actual.kind != static_cast<uint8_t>(expected.kind) ||
        actual.schema_version != expected.schema_version ||
        actual.profile_version != expected.profile_version || actual.target_id != expected.target_id ||
        actual.hash != expected.hash) return fail("RESULT profile identity changed");
  }
  telemetry_archive::ImmutableBatch batch{};
  if (!telemetry_archive::BatchJsonBuilder{}.build(record, batch)) return fail("RESULT profile JSON building");
  std::cout << "PROFILES " << batch.exact_body() << std::endl;
  return 0;
}
bool rebuild_payload(const storage::ImmutableRecordBytes& input, std::vector<uint8_t> payload,
                     storage::ImmutableRecordBytes& output) {
  storage::ArchiveRecordView view{};
  storage::PayloadWriter writer{nullptr, 0U};
  return storage::RecordCodec::read(input, view) &&
      storage::RecordCodec::begin(view.identity, output, writer) &&
      writer.bytes(payload.data(), payload.size()) && storage::RecordCodec::finish(output, writer);
}
int flags(SigapApp& app) {
  std::array<bool, 13U> seen{};
  unsigned negatives{};
  for (uint64_t now = 0U; now <= 6200000U; now += 1000U) {
    test_platform::now_us = now; DebugAppProbe::acquire(app, now);
    SampleRecord sample{};
    while (DebugAppProbe::pop(app, sample)) {
      const auto id = static_cast<std::size_t>(sample.header.sensor);
      if (seen[id] || (sample.header.sensor != SensorId::Mq7 &&
          sample.header.sensor != SensorId::Sht31_44 && sample.header.sensor != SensorId::Sht31_45)) continue;
      storage::ImmutableRecordBytes encoded{};
      storage::ArchiveRecordView view{};
      SampleRecord decoded{};
      if (!storage::RecordCodec::encode_sample(sample, encoded) ||
          !storage::RecordCodec::decode_sample(encoded, decoded) ||
          !storage::RecordCodec::read(encoded, view)) return fail("positive evidence frame");
      const std::vector<uint8_t> original(view.payload, view.payload + view.payload_size);
      const std::size_t extension = original.size() - storage::kArchiveSampleExtensionBytes;
      if (original[extension] != 0xa1U || original[extension + 1U] != 1U)
        return fail("extension fixture tag/version");
      for (const std::size_t field : {2U, 4U}) {
        for (const uint8_t invalid : {2U, 255U}) {
          auto payload = original; payload[extension + field] = invalid;
          storage::ImmutableRecordBytes altered{};
          storage::ArchiveRecordView valid_frame{};
          if (!rebuild_payload(encoded, payload, altered) || !storage::RecordCodec::read(altered, valid_frame))
            return fail("negative must retain valid framing/CRC");
          if (storage::RecordCodec::decode_sample(altered, decoded))
            return fail("noncanonical evidence availability flag accepted");
          ++negatives;
        }
      }
      seen[id] = true;
    }
  }
  if (negatives != 12U) return fail("MQ7 and both SHT addresses must exercise invalid flags");
  std::puts("PASS strict_flags valid_frames=3 canonical_flags=0,1 negative_frames=12");
  return 0;
}
int ds(SigapApp& app) {
  IdentityFixture ids;
  RecordFactory factory(ids.sequence, {ids.boot, ids.device, ids.config, nullptr});
  auto profile = DebugAppProbe::ds(app);
  // Synthetic valid-profile control, not a physical calibration or app-profile mutation.
  profile.manifest.validity = ProfileValidity::Valid;
  EncodedProfile encoded_profile{};
  if (!ProfileCodec{}.encode(profile, encoded_profile)) return fail("synthetic DS profile codec");
  profile.manifest.hash = encoded_profile.hash();
  SimulatedDs18b20Reader reader(2U, &profile);
  if (!reader.begin()) return fail("generated DS source begin");
  Ds18b20Acquirer acquirer(reader, factory, &profile, ds18b20_conversion_time_us(profile.resolution_bits));
  SampleRecord sample{};
  reader.set_now(0U);
  if (acquirer.poll(0U, sample) != DriverPollStatus::NoAttempt) return fail("DS async conversion start");
  const auto ready_at = ds18b20_conversion_time_us(profile.resolution_bits);
  reader.set_now(ready_at);
  if (acquirer.poll(ready_at, sample) != DriverPollStatus::Produced || !sample.converted.primary_available ||
      sample.converted.primary_kind != ConvertedKind::Celsius) return fail("DS positive converted acquisition");
  std::cout << "DS_EXPECTED " << sample.raw.temperature_centi_c + profile.offset_centi_c << std::endl;
  for (const bool available : {true, false}) {
    auto control = sample;
    if (!available) { control.converted.primary_available = false; control.quality = SampleQuality::Uncalibrated; }
    storage::ImmutableRecordBytes encoded{};
    SampleRecord decoded{};
    telemetry_archive::ImmutableBatch batch{};
    if (!storage::RecordCodec::encode_sample(control, encoded) ||
        !storage::RecordCodec::decode_sample(encoded, decoded) ||
        !telemetry_archive::BatchJsonBuilder{}.build(encoded, batch) ||
        decoded.raw.ds_rom != profile.rom || !decoded.raw.ds_wire_evidence ||
        ds18b20_crc8(decoded.raw.ds_rom.data(), 7U) != decoded.raw.ds_rom[7] ||
        ds18b20_crc8(decoded.raw.ds_scratchpad.data(), 8U) != decoded.raw.ds_scratchpad[8] ||
        decoded.raw.ds_resolution_bits != profile.resolution_bits)
      return fail("DS raw/converted durable builder");
    std::cout << (available ? "DS_VALUE " : "DS_NULL ") << batch.exact_body() << std::endl;
    if (batch.exact_body().find("\"temperature_c\":") == std::string_view::npos)
      return fail("DS converted temperature field absent");
  }
  return 0;
}
}
int main(int argc, char** argv) {
  if (argc != 2) return fail("expected head/profiles/flags/ds");
  seed_test_boot();
  sigap::SigapApp app;
  app.setup();
  if (std::strcmp(argv[1], "head") == 0) return head(app);
  if (std::strcmp(argv[1], "profiles") == 0) return profiles();
  if (std::strcmp(argv[1], "flags") == 0) return flags(app);
  if (std::strcmp(argv[1], "ds") == 0) return ds(app);
  return fail("unknown probe");
}
