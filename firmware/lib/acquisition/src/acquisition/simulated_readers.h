#pragma once

#include <array>
#include <cstdint>

#include "acquisition/heater.h"
#include "acquisition/target_protocol.h"
#include "acquisition/sensors/hx711.h"
#include "acquisition/sensors/mq_adc.h"
#include "acquisition/sensors/ultrasonic.h"
#include "acquisition/sensors/ds18b20.h"
#include "acquisition/sensors/sht31.h"

namespace sigap::acquisition {

// These are deterministic virtual raw inputs, not calibrated physical models.
// No class in this file accesses Arduino GPIO, Wire, NVS or a heater output.
class SimulatedHx711Reader final : public IHx711Reader {
 public:
  SimulatedHx711Reader(uint8_t channel, uint8_t) noexcept : channel_(channel) {}
  bool ready() noexcept override { return true; }
  Hx711ReadResult read() noexcept override {
    const int64_t counts = -20000LL + static_cast<int64_t>(channel_) * 100LL +
                           static_cast<int64_t>(step_++ % 200U) * 211LL;
    return {true, false, counts, 1U, 0U, SampleReason::DataReady};
  }
  void reset() noexcept { step_ = 0U; }
 private:
  uint8_t channel_;
  uint32_t step_{0U};
};

class SimulatedMqAdcReader final : public IMqAdcReader {
 public:
  explicit SimulatedMqAdcReader(uint8_t channel) noexcept : channel_(channel) {}
  MqAdcReadResult read() noexcept override {
    const auto counts = static_cast<uint16_t>(
        (500U + channel_ * 83U + (step_++ % 200U) * 11U) % 4096U);
    return {true, false, counts, SampleReason::DataReady};
  }
  void reset() noexcept { step_ = 0U; }
 private:
  uint8_t channel_;
  uint32_t step_{0U};
};

class SimulatedUltrasonicReader final : public IUltrasonicReader {
 public:
  SimulatedUltrasonicReader(uint8_t channel, uint8_t) noexcept : channel_(channel) {}
  bool trigger(uint64_t now_us) noexcept override {
    if (active_) return false;
    started_us_ = now_us;
    width_us_ = 1000U + channel_ * 7U + (step_++ % 20U) * 17U;
    active_ = true;
    return true;
  }
  UltrasonicEchoResult poll(uint64_t now_us) noexcept override {
    if (!active_) return {};
    if (now_us < started_us_) { cancel(); return {UltrasonicEchoStatus::Timeout, 0U}; }
    if (now_us - started_us_ < width_us_) return {};
    active_ = false;
    return {UltrasonicEchoStatus::Ready, width_us_};
  }
  void cancel() noexcept override { active_ = false; }
  void reset() noexcept { active_ = false; step_ = 0U; }
 private:
  uint8_t channel_;
  uint32_t step_{0U};
  uint32_t width_us_{0U};
  uint64_t started_us_{0U};
  bool active_{false};
};

class SimulatedDs18b20Reader final : public IDs18b20Reader {
 public:
  SimulatedDs18b20Reader(uint8_t, const Ds18b20Profile* profile = nullptr) noexcept
      : profile_(profile) {}
  void set_profile(const Ds18b20Profile* profile) noexcept {
    profile_ = profile; requested_ = false; step_ = 0U;
  }
  void set_now(uint64_t now_us) noexcept { now_us_ = now_us; }
  bool begin() noexcept override {
    return profile_ != nullptr && profile_->valid_for_acquisition() &&
           profile_matches(profile_->manifest, SensorId::Ds18b20B3) &&
           ds18b20_crc8(profile_->rom.data(), 7U) == profile_->rom[7];
  }
  bool apply_profile(const Ds18b20Profile* profile) noexcept override {
    set_profile(profile); return begin();
  }
  bool request_conversion() noexcept override {
    if (requested_ || !begin()) return false;
    requested_at_us_ = now_us_;
    resolution_ = profile_->resolution_bits;
    requested_ = true;
    return true;
  }
  bool conversion_ready() noexcept override {
    return requested_ && now_us_ >= requested_at_us_ &&
           now_us_ - requested_at_us_ >= ds18b20_conversion_time_us(resolution_);
  }
  Ds18b20ReadResult read_conversion() noexcept override {
    if (!conversion_ready()) return {};
    requested_ = false;
    const uint16_t temperature = static_cast<uint16_t>(400U + (++step_ % 16U) * 8U);
    std::array<uint8_t, 9> frame{
        static_cast<uint8_t>(temperature), static_cast<uint8_t>(temperature >> 8U),
        0x4bU, 0x46U, ds18b20_resolution_config(resolution_), 0xffU, 0x0cU, 0x10U, 0U};
    frame[8] = ds18b20_crc8(frame.data(), 8U);
    int32_t temperature_centi_c{0};
    const bool crc = decode_ds18b20_scratchpad(frame, temperature_centi_c);
    Ds18b20ReadResult result{};
    result.present = true; result.crc_valid = crc; result.conversion_complete = true;
    result.temperature_centi_c = temperature_centi_c;
    result.resolution_matches = true; result.identity_valid = begin();
    // These bytes are generated protocol input, not physical bus capture.
    result.wire_evidence = true;
    result.rom = profile_->rom;
    result.scratchpad = frame;
    result.resolution_bits = resolution_;
    return result;
  }
 private:
  const Ds18b20Profile* profile_;
  uint64_t now_us_{0U};
  uint64_t requested_at_us_{0U};
  uint32_t step_{0U};
  uint8_t resolution_{12U};
  bool requested_{false};
};

class SimulatedSht31Reader final : public ISht31Reader {
 public:
  SimulatedSht31Reader(uint8_t, uint8_t) noexcept {}
  void set_now(uint64_t now_us) noexcept { now_us_ = now_us; }
  void reset() noexcept { channels_ = {}; }
  Sht31ReadResult read(uint8_t address) noexcept override {
    if (address != 0x44U && address != 0x45U) return {};
    auto& channel = channels_[address == 0x45U ? 1U : 0U];
    if (!channel.pending) {
      channel.pending = true; channel.started_us = now_us_;
      Sht31ReadResult result{}; result.pending = true; return result;
    }
    if (now_us_ < channel.started_us || now_us_ - channel.started_us < 15000U) {
      Sht31ReadResult result{}; result.pending = true; return result;
    }
    channel.pending = false;
    const auto temperature = static_cast<uint16_t>(26000U + (address - 0x44U) * 400U +
                                                  (++channel.step % 20U) * 113U);
    const auto humidity = static_cast<uint16_t>(32000U + (channel.step % 20U) * 71U);
    std::array<uint8_t, 6> frame{
        static_cast<uint8_t>(temperature >> 8U), static_cast<uint8_t>(temperature), 0U,
        static_cast<uint8_t>(humidity >> 8U), static_cast<uint8_t>(humidity), 0U};
    frame[2] = sht31_crc8(frame.data(), 2U);
    frame[5] = sht31_crc8(frame.data() + 3U, 2U);
    const auto decoded = decode_sht31_frame(frame);
    Sht31ReadResult result{};
    result.ok = true;
    result.temperature_crc_valid = decoded.temperature_crc_valid;
    result.humidity_crc_valid = decoded.humidity_crc_valid;
    result.crc_valid = result.temperature_crc_valid && result.humidity_crc_valid;
    result.temperature_centi_c = decoded.temperature_centi_c;
    result.humidity_centi_pct = decoded.humidity_centi_pct;
    result.temperature_ticks = temperature;
    result.humidity_ticks = humidity;
    return result;
  }
 private:
  struct Channel { uint64_t started_us{0U}; uint32_t step{0U}; bool pending{false}; };
  std::array<Channel, 2> channels_{};
  uint64_t now_us_{0U};
};

class SimulatedHeaterRail final : public IMq7HeaterRail {
 public:
  explicit SimulatedHeaterRail(uint8_t) noexcept {}
  // The heater is deliberately unavailable, even as a virtual readiness claim.
  bool select_high() noexcept override { return false; }
  bool select_low() noexcept override { return false; }
};

}  // namespace sigap::acquisition
