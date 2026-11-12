#include <array>
#include <cstdio>
#include <cstring>
#include "acquisition/target_runtime.h"
#include "storage/archive_codec.h"
#include "sigap/build/build_config.h"
using namespace sigap::acquisition;
int fail(const char* message) { std::fprintf(stderr, "FAIL %s\n", message); return 1; }
int signed_counts() {
  TargetHx711Reader reader(10, 11);
  for (const auto& pair : std::array<std::pair<uint32_t, int64_t>, 4>{{
       {0xffffffU, -1}, {0x800000U, -8388608}, {0x7fffffU, 8388607}, {0U, 0}}}) {
    sensor_io::hx_frame = pair.first; sensor_io::hx_cursor = 0;
    const auto value = reader.read();
    if (!value.ok || value.counts != pair.second) {
      std::fprintf(stderr, "frame=%u actual=%lld expected=%lld\n", pair.first,
                   static_cast<long long>(value.counts), static_cast<long long>(pair.second));
      return fail("physical HX711 signed24 decode");
    }
  }
  std::puts("PASS physical HX711 signed24 boundaries"); return 0;
}
int selection(bool simulation) {
  BootId boot{}; boot.present = true; boot.bytes[0] = 1U;
  DeviceId device{}; device.present = true; device.bytes[0] = 1U;
  ConfigRef config{}; config.present = true; config.schema_version = 1U;
  config.config_version = 1U; config.hash[0] = 1U;
  SequenceAllocator sequence;
  RecordFactory factory(sequence, RecordContext{boot, device, config, nullptr});
  Ds18b20Profile ds{};
  ds.manifest.validity = ProfileValidity::TestOnly;
  ds.manifest.target_id = static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  ds.has_rom = true; ds.rom[0] = 0x28U; ds.rom[1] = 0x01U;
  ds.rom[7] = ds18b20_crc8(ds.rom.data(), 7U);
  TargetProfileSet profiles{}; profiles.ds18b20 = &ds;
  SampleRing<TargetAcquisition::kHandoffCapacity> handoff;
  sensor_io::calls = 0;
  TargetAcquisition acquisition(factory, sigap::board::kN16R8Pins, profiles, handoff);
  if (simulation && sensor_io::calls != 0) return fail("SIM constructed physical GPIO/I2C adapters");
  if (!simulation) {
    if (sensor_io::calls == 0) return fail("OFF did not construct physical readers");
    if (std::strcmp(sigap::build::kFirmwareBuildId, "sigap-task6") != 0) return fail("OFF boot tag changed");
    std::puts("PASS default OFF selects physical readers"); return 0;
  }
  if (std::strcmp(sigap::build::kFirmwareBuildId, "sigap-task6-sensor-sim-v1") != 0) return fail("SIM boot tag missing");
  if (!acquisition.begin(sigap::config::SamplingConfig{}, 0U)) return fail("SIM graph begin");
  if (acquisition.registered_jobs() != 14U) return fail("normal scheduler job count");
  std::array<unsigned, 13> seen{};
  std::array<int64_t, 13> first{}, last{};
  uint64_t expected_sequence = 0;
  for (uint64_t now = 0; now <= 6200000U; now += 1000U) {
    sensor_io::now_us = now; acquisition.poll(now);
    SampleRecord sample;
    while (handoff.try_pop(sample) == HandoffStatus::Dequeued) {
      const auto index = static_cast<std::size_t>(sample.header.sensor);
      if (index >= seen.size()) return fail("sensor identity");
      if (sample.header.sequence != expected_sequence++) return fail("global sequence");
      if (!sample.header.config_ref.present || !sample.header.boot_id.present) return fail("identity provenance");
      if (!sample.raw.present) return fail("scheduled simulated channel has no raw observation");
      sigap::storage::ImmutableRecordBytes durable{};
      SampleRecord restored{};
      if (!sigap::storage::RecordCodec::encode_sample(sample, durable) ||
          !sigap::storage::RecordCodec::decode_sample(durable, restored))
        return fail("acquisition sample durable archive roundtrip");
      if (restored.raw.heater_phase_available != sample.raw.heater_phase_available ||
          restored.raw.heater_phase != sample.raw.heater_phase ||
          restored.raw.sht31_ticks_available != sample.raw.sht31_ticks_available ||
          restored.raw.sht31_temperature_ticks != sample.raw.sht31_temperature_ticks ||
          restored.raw.sht31_humidity_ticks != sample.raw.sht31_humidity_ticks ||
          restored.raw.ds_rom != sample.raw.ds_rom ||
          restored.raw.ds_scratchpad != sample.raw.ds_scratchpad ||
          restored.raw.ds_resolution_bits != sample.raw.ds_resolution_bits)
        return fail("durable codec discarded producer protocol evidence");

      if (index == 10U) {
        if (!sample.raw.ds_wire_evidence || sample.raw.ds_rom != ds.rom ||
            ds18b20_crc8(sample.raw.ds_rom.data(), 7U) != sample.raw.ds_rom[7] ||
            ds18b20_crc8(sample.raw.ds_scratchpad.data(), 8U) != sample.raw.ds_scratchpad[8] ||
            sample.raw.ds_resolution_bits != ds.resolution_bits)
          return fail("generated SIM DS ROM/scratchpad CRC/resolution evidence");
      }
      if ((index == 11U || index == 12U) && !sample.raw.sht31_ticks_available)
        return fail("SHT31 original frame ticks unavailable");
      int64_t raw = sample.raw.adc_counts;
      if (index >= 4U && index <= 6U) {
        raw = sample.raw.signed_counts;
        if (raw < -8388608LL || raw > 8388607LL) return fail("HX signed24 range");
      } else if (index >= 7U && index <= 9U) {
        raw = sample.raw.distance_us;
        if (raw <= 0 || raw > 30000) return fail("ultrasonic echo range");
      } else if (index >= 10U) {
        raw = sample.raw.temperature_centi_c;
        if (index >= 11U && (sample.raw.humidity_centi_pct < 0 || sample.raw.humidity_centi_pct > 10000)) return fail("SHT humidity range");
      } else if (raw > 4095) return fail("MQ ADC range");
      if (seen[index]++ == 0U) first[index] = raw;
      last[index] = raw;
      const auto expected_stream = (index == 2U || index == 3U || index == 6U || index == 9U || index == 10U || index == 12U)
          ? StreamId::B3 : (index == 5U || index == 8U) ? StreamId::Inorganic : StreamId::Organic;
      if (sample.header.stream != expected_stream) return fail("sensor/stream mapping");
    }
  }
  for (std::size_t index = 0; index < seen.size(); ++index)
    if (seen[index] == 0 || first[index] == last[index]) return fail("all13 channels must produce evolving raw values");
  if (sensor_io::calls != 0U) return fail("SIM performed physical sensor/heater I/O");
  if (handoff.dropped_records() != 0U) return fail("unexpected sample loss");
  acquisition.disable(); acquisition.poll(8000000U);
  if (handoff.size() != 0U) return fail("disabled simulation still produces");
  std::printf("PASS SIM channels=13 records=%llu physicalIO=0 normalJobs=14\n", static_cast<unsigned long long>(expected_sequence));
  return 0;
}
int main(int argc, char** argv) {
  if (argc != 2) return fail("expected sim/off/signed");
  if (std::strcmp(argv[1], "signed") == 0) return signed_counts();
  return selection(std::strcmp(argv[1], "sim") == 0);
}
