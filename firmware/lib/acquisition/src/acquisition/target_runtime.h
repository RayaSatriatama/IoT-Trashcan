#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "sigap/board/board_pins.h"
#include "config/config_types.h"
#include "acquisition/handoff.h"
#include "acquisition/health.h"
#include "acquisition/heater.h"
#include "acquisition/record.h"
#include "acquisition/scheduler.h"
#include "acquisition/sensors/ds18b20.h"
#include "acquisition/sensors/hx711.h"
#include "acquisition/sensors/mq_adc.h"
#include "acquisition/sensors/sht31.h"
#include "acquisition/sensors/ultrasonic.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace sigap::acquisition {

// The graph receives interfaces rather than concrete sensor libraries. This
// keeps the composition root explicit and makes the same graph testable with
// native fakes.
struct TargetReaderSet {
  std::array<IHx711Reader*, 3> hx711{};
  std::array<IUltrasonicReader*, 3> ultrasonic{};
  std::array<IMqAdcReader*, 4> mq_adc{};
  IDs18b20Reader* ds18b20{nullptr};
  std::array<ISht31Reader*, 2> sht31{};
  IMq7HeaterRail* heater_rail{nullptr};
};

struct TargetProfileSet {
  std::array<const Hx711Profile*, 3> hx711{};
  std::array<const UltrasonicProfile*, 3> ultrasonic{};
  std::array<const MqAdcProfile*, 4> mq_adc{};
  const Ds18b20Profile* ds18b20{nullptr};
  std::array<const Sht31Profile*, 2> sht31{};
  const Mq7HeaterProfile* heater{nullptr};
};

template <typename Acquirer, std::size_t Capacity>
struct AcquisitionJob final {
  Acquirer* acquirer{nullptr};
  SampleRing<Capacity>* handoff{nullptr};
  SensorHealthState* health{nullptr};

  static void run(void* context, uint64_t now_us) noexcept {
    auto* job = static_cast<AcquisitionJob*>(context);
    if (job == nullptr || job->acquirer == nullptr || job->handoff == nullptr) {
      return;
    }
    SampleRecord record{};
    if (job->acquirer->poll(now_us, record) == DriverPollStatus::Produced) {
      if (job->health != nullptr) {
        job->health->observe(now_us, record.quality);
      }
      (void)job->handoff->try_push(record);
    }
  }
};

struct HeaterJob final {
  Mq7HeaterController* controller{nullptr};

  static void run(void* context, uint64_t now_us) noexcept {
    auto* job = static_cast<HeaterJob*>(context);
    if (job != nullptr && job->controller != nullptr) {
      job->controller->tick(now_us);
    }
  }
};

class TargetAcquisitionGraph final {
 public:
  static constexpr std::size_t kHandoffCapacity{16};
  static constexpr std::size_t kJobCount{14};
  static constexpr uint64_t kUltrasonicTimeoutUs{30000};
  static constexpr uint64_t kDs18b20ConversionUs{750000};

  TargetAcquisitionGraph(RecordFactory& factory,
                         const TargetReaderSet& readers,
                         const TargetProfileSet& profiles,
                         SampleRing<kHandoffCapacity>& handoff) noexcept
      : factory_(factory), readers_(readers), profiles_(profiles), handoff_(handoff),
        heater_profile_(profiles.heater != nullptr ? *profiles.heater : Mq7HeaterProfile{}),
        heater_(*readers.heater_rail, heater_profile_),
        hx_org_(SensorId::Hx711Organic, StreamId::Organic, *readers.hx711[0], factory_, profiles.hx711[0]),
        hx_inorganic_(SensorId::Hx711Inorganic, StreamId::Inorganic, *readers.hx711[1], factory_, profiles.hx711[1]),
        hx_b3_(SensorId::Hx711B3, StreamId::B3, *readers.hx711[2], factory_, profiles.hx711[2]),
        us_org_(StreamId::Organic, *readers.ultrasonic[0], factory_, profiles.ultrasonic[0], kUltrasonicTimeoutUs, us_gate_),
        us_inorganic_(StreamId::Inorganic, *readers.ultrasonic[1], factory_, profiles.ultrasonic[1], kUltrasonicTimeoutUs, us_gate_),
        us_b3_(StreamId::B3, *readers.ultrasonic[2], factory_, profiles.ultrasonic[2], kUltrasonicTimeoutUs, us_gate_),
        mq4_(SensorId::Mq4, StreamId::Organic, *readers.mq_adc[0], factory_, profiles.mq_adc[0]),
        mq135_(SensorId::Mq135, StreamId::Organic, *readers.mq_adc[1], factory_, profiles.mq_adc[1]),
        mq7_(SensorId::Mq7, StreamId::B3, *readers.mq_adc[2], factory_, profiles.mq_adc[2], &heater_),
        mq8_(SensorId::Mq8, StreamId::B3, *readers.mq_adc[3], factory_, profiles.mq_adc[3]),
        ds18b20_(*readers.ds18b20, factory_, profiles.ds18b20, kDs18b20ConversionUs),
        sht31_44_(0x44U, StreamId::Ambient, *readers.sht31[0], factory_, profiles.sht31[0]),
        sht31_45_(0x45U, StreamId::Ambient, *readers.sht31[1], factory_, profiles.sht31[1]) {
    hx_jobs_[0] = {&hx_org_, &handoff_, &health_[0]};
    hx_jobs_[1] = {&hx_inorganic_, &handoff_, &health_[1]};
    hx_jobs_[2] = {&hx_b3_, &handoff_, &health_[2]};
    us_jobs_[0] = {&us_org_, &handoff_, &health_[3]};
    us_jobs_[1] = {&us_inorganic_, &handoff_, &health_[4]};
    us_jobs_[2] = {&us_b3_, &handoff_, &health_[5]};
    mq_jobs_[0] = {&mq4_, &handoff_, &health_[6]};
    mq_jobs_[1] = {&mq135_, &handoff_, &health_[7]};
    mq_jobs_[2] = {&mq7_, &handoff_, &health_[8]};
    mq_jobs_[3] = {&mq8_, &handoff_, &health_[9]};
    heater_job_.controller = &heater_;
    ds_job_ = {&ds18b20_, &handoff_, &health_[10]};
    sht_jobs_[0] = {&sht31_44_, &handoff_, &health_[11]};
    sht_jobs_[1] = {&sht31_45_, &handoff_, &health_[12]};
  }

  bool begin(const config::SamplingConfig& sampling, uint64_t first_due_us) noexcept {
    if (enabled_ || readers_.heater_rail == nullptr || readers_.ds18b20 == nullptr ||
        !all_readers_present()) {
      return false;
    }
    const uint64_t hx_period = milliseconds_to_us(sampling.hx711_period_ms);
    const uint64_t mq_period = milliseconds_to_us(sampling.mq_period_ms);
    const uint64_t ultrasonic_period = milliseconds_to_us(sampling.ultrasonic_period_ms);
    const uint64_t ds_period = milliseconds_to_us(sampling.ds18b20_period_ms);
    const uint64_t sht_period = milliseconds_to_us(sampling.sht31_period_ms);
    if (hx_period == 0U || mq_period == 0U || ultrasonic_period == 0U ||
        ds_period == 0U || sht_period == 0U) {
      return false;
    }

    bool ok = true;
    ok = add(hx_period, &AcquisitionJob<Hx711Acquirer, kHandoffCapacity>::run, &hx_jobs_[0], first_due_us) && ok;
    ok = add(hx_period, &AcquisitionJob<Hx711Acquirer, kHandoffCapacity>::run, &hx_jobs_[1], first_due_us) && ok;
    ok = add(hx_period, &AcquisitionJob<Hx711Acquirer, kHandoffCapacity>::run, &hx_jobs_[2], first_due_us) && ok;
    ok = add(ultrasonic_period, &AcquisitionJob<UltrasonicAcquirer, kHandoffCapacity>::run, &us_jobs_[0], first_due_us) && ok;
    ok = add(ultrasonic_period, &AcquisitionJob<UltrasonicAcquirer, kHandoffCapacity>::run, &us_jobs_[1], first_due_us) && ok;
    ok = add(ultrasonic_period, &AcquisitionJob<UltrasonicAcquirer, kHandoffCapacity>::run, &us_jobs_[2], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[0], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[1], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[2], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[3], first_due_us) && ok;
    ok = add(ds_period, &AcquisitionJob<Ds18b20Acquirer, kHandoffCapacity>::run, &ds_job_, first_due_us) && ok;
    ok = add(sht_period, &AcquisitionJob<Sht31Acquirer, kHandoffCapacity>::run, &sht_jobs_[0], first_due_us) && ok;
    ok = add(sht_period, &AcquisitionJob<Sht31Acquirer, kHandoffCapacity>::run, &sht_jobs_[1], first_due_us) && ok;
    heater_.start(first_due_us);
    ok = add(mq_period, &HeaterJob::run, &heater_job_, first_due_us) && ok;
    enabled_ = ok;
    return enabled_;
  }

  void poll(uint64_t now_us) noexcept {
    if (enabled_) {
      scheduler_.poll(now_us);
    }
  }

  void disable() noexcept { enabled_ = false; }

  bool enabled() const noexcept { return enabled_; }
  std::size_t registered_jobs() const noexcept { return registered_jobs_; }
  SampleRing<kHandoffCapacity>& handoff() noexcept { return handoff_; }
  const SampleRing<kHandoffCapacity>& handoff() const noexcept { return handoff_; }
  const Mq7HeaterController& heater() const noexcept { return heater_; }
  const SensorHealthState& health(std::size_t index) const noexcept {
    return health_[index < health_.size() ? index : 0U];
  }

 private:
  static uint64_t milliseconds_to_us(uint32_t milliseconds) noexcept {
    constexpr uint64_t kMaxMs = UINT64_MAX / 1000U;
    if (static_cast<uint64_t>(milliseconds) > kMaxMs) return 0U;
    return static_cast<uint64_t>(milliseconds) * 1000U;
  }

  bool all_readers_present() const noexcept {
    const auto present = [](const auto* reader) { return reader != nullptr; };
    return std::all_of(readers_.hx711.begin(), readers_.hx711.end(), present) &&
           std::all_of(readers_.ultrasonic.begin(), readers_.ultrasonic.end(), present) &&
           std::all_of(readers_.mq_adc.begin(), readers_.mq_adc.end(), present) &&
           std::all_of(readers_.sht31.begin(), readers_.sht31.end(), present);
  }

  bool add(uint64_t period_us, CooperativeScheduler::Callback callback,
           void* context, uint64_t first_due_us) noexcept {
    if (!scheduler_.add_job(period_us, callback, context, first_due_us)) return false;
    ++registered_jobs_;
    return true;
  }

  RecordFactory& factory_;
  const TargetReaderSet& readers_;
  const TargetProfileSet& profiles_;
  SampleRing<kHandoffCapacity>& handoff_;
  Mq7HeaterProfile heater_profile_{};
  Mq7HeaterController heater_;
  UltrasonicTriggerGate us_gate_{};
  Hx711Acquirer hx_org_;
  Hx711Acquirer hx_inorganic_;
  Hx711Acquirer hx_b3_;
  UltrasonicAcquirer us_org_;
  UltrasonicAcquirer us_inorganic_;
  UltrasonicAcquirer us_b3_;
  MqAdcAcquirer mq4_;
  MqAdcAcquirer mq135_;
  MqAdcAcquirer mq7_;
  MqAdcAcquirer mq8_;
  Ds18b20Acquirer ds18b20_;
  Sht31Acquirer sht31_44_;
  Sht31Acquirer sht31_45_;
  std::array<AcquisitionJob<Hx711Acquirer, kHandoffCapacity>, 3> hx_jobs_{};
  std::array<AcquisitionJob<UltrasonicAcquirer, kHandoffCapacity>, 3> us_jobs_{};
  std::array<AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>, 4> mq_jobs_{};
  AcquisitionJob<Ds18b20Acquirer, kHandoffCapacity> ds_job_{};
  std::array<AcquisitionJob<Sht31Acquirer, kHandoffCapacity>, 2> sht_jobs_{};
  HeaterJob heater_job_{};
  std::array<SensorHealthState, 13> health_{};
  CooperativeScheduler scheduler_{};
  std::size_t registered_jobs_{0};
  bool enabled_{false};
};

// Minimal target GPIO adapters. They intentionally expose only bounded,
// non-blocking seams. Sensor libraries can replace these adapters later
// without changing graph ownership or scheduling.
#ifdef ARDUINO
class TargetMqAdcReader final : public IMqAdcReader {
 public:
  explicit TargetMqAdcReader(uint8_t pin) noexcept : pin_(pin) { pinMode(pin_, INPUT); }
  MqAdcReadResult read() noexcept override {
    const int value = analogRead(pin_);
    if (value < 0) return MqAdcReadResult{false, true, 0U, SampleReason::Disconnected};
    return MqAdcReadResult{true, false, static_cast<uint16_t>(value), SampleReason::DataReady};
  }

 private:
  uint8_t pin_;
};

class TargetHx711Reader final : public IHx711Reader {
 public:
  TargetHx711Reader(uint8_t dout, uint8_t sck) noexcept : dout_(dout), sck_(sck) {
    pinMode(dout_, INPUT);
    pinMode(sck_, OUTPUT);
    digitalWrite(sck_, LOW);
  }
  bool ready() noexcept override { return digitalRead(dout_) == LOW; }
  Hx711ReadResult read() noexcept override {
    if (!ready()) return Hx711ReadResult{false, false, 0, 0, 0, SampleReason::NotReady};
    uint32_t value = 0;
    for (uint8_t bit = 0; bit < 24U; ++bit) {
      digitalWrite(sck_, HIGH);
      delayMicroseconds(1U);
      value = (value << 1U) | (digitalRead(dout_) == HIGH ? 1U : 0U);
      digitalWrite(sck_, LOW);
      delayMicroseconds(1U);
    }
    digitalWrite(sck_, HIGH);
    delayMicroseconds(1U);
    digitalWrite(sck_, LOW);
    const int64_t signed_value = (value & 0x800000U) != 0U
                                      ? static_cast<int64_t>(value | 0xff000000U)
                                      : static_cast<int64_t>(value);
    return Hx711ReadResult{true, false, signed_value, 1U, 0U, SampleReason::DataReady};
  }

 private:
  uint8_t dout_;
  uint8_t sck_;
};

class TargetUltrasonicReader final : public IUltrasonicReader {
 public:
  TargetUltrasonicReader(uint8_t trigger, uint8_t echo) noexcept
      : trigger_(trigger), echo_(echo) {
    pinMode(trigger_, OUTPUT);
    pinMode(echo_, INPUT);
    digitalWrite(trigger_, LOW);
  }
  bool trigger(uint64_t now_us) noexcept override {
    if (active_) return false;
    digitalWrite(trigger_, HIGH);
    delayMicroseconds(10U);
    digitalWrite(trigger_, LOW);
    active_ = true;
    started_us_ = now_us;
    return true;
  }
  UltrasonicEchoResult poll(uint64_t now_us) noexcept override {
    if (!active_) return UltrasonicEchoResult{UltrasonicEchoStatus::Pending, 0U};
    if (now_us - started_us_ >= 30000U) {
      active_ = false;
      echo_high_ = false;
      return UltrasonicEchoResult{UltrasonicEchoStatus::Timeout, 0U};
    }
    if (digitalRead(echo_) == HIGH) {
      if (!echo_high_) {
        echo_high_ = true;
        echo_started_us_ = now_us;
      }
      return UltrasonicEchoResult{UltrasonicEchoStatus::Pending, 0U};
    }
    if (echo_high_) {
      const uint64_t width = now_us - echo_started_us_;
      active_ = false;
      echo_high_ = false;
      return UltrasonicEchoResult{
          UltrasonicEchoStatus::Ready,
          width > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(width)};
    }
    return UltrasonicEchoResult{UltrasonicEchoStatus::Pending, 0U};
  }

 private:
  uint8_t trigger_;
  uint8_t echo_;
  bool active_{false};
  bool echo_high_{false};
  uint64_t started_us_{0};
  uint64_t echo_started_us_{0};
};

class TargetDs18b20Reader final : public IDs18b20Reader {
 public:
  explicit TargetDs18b20Reader(uint8_t data_pin) noexcept : data_pin_(data_pin) {
    pinMode(data_pin_, INPUT_PULLUP);
  }
  bool request_conversion() noexcept override { return false; }
  Ds18b20ReadResult read_conversion() noexcept override { return Ds18b20ReadResult{}; }

 private:
  uint8_t data_pin_;
};

class TargetSht31Reader final : public ISht31Reader {
 public:
  TargetSht31Reader(uint8_t sda, uint8_t scl) noexcept : sda_(sda), scl_(scl) {}
  Sht31ReadResult read(uint8_t) noexcept override { return Sht31ReadResult{}; }

 private:
  uint8_t sda_;
  uint8_t scl_;
};

class TargetHeaterRail final : public IMq7HeaterRail {
 public:
  explicit TargetHeaterRail(uint8_t select_pin) noexcept : select_pin_(select_pin) {
    pinMode(select_pin_, OUTPUT);
    digitalWrite(select_pin_, LOW);
  }
  bool select_high() noexcept override { digitalWrite(select_pin_, HIGH); return true; }
  bool select_low() noexcept override { digitalWrite(select_pin_, LOW); return true; }

 private:
  uint8_t select_pin_;
};
#else
class TargetMqAdcReader final : public IMqAdcReader {
 public:
  explicit TargetMqAdcReader(uint8_t) noexcept {}
  MqAdcReadResult read() noexcept override { return MqAdcReadResult{}; }
};
class TargetHx711Reader final : public IHx711Reader {
 public:
  TargetHx711Reader(uint8_t, uint8_t) noexcept {}
  bool ready() noexcept override { return false; }
  Hx711ReadResult read() noexcept override { return Hx711ReadResult{}; }
};
class TargetUltrasonicReader final : public IUltrasonicReader {
 public:
  TargetUltrasonicReader(uint8_t, uint8_t) noexcept {}
  bool trigger(uint64_t) noexcept override { return false; }
  UltrasonicEchoResult poll(uint64_t) noexcept override { return UltrasonicEchoResult{}; }
};
class TargetDs18b20Reader final : public IDs18b20Reader {
 public:
  explicit TargetDs18b20Reader(uint8_t) noexcept {}
  bool request_conversion() noexcept override { return false; }
  Ds18b20ReadResult read_conversion() noexcept override { return Ds18b20ReadResult{}; }
};
class TargetSht31Reader final : public ISht31Reader {
 public:
  TargetSht31Reader(uint8_t, uint8_t) noexcept {}
  Sht31ReadResult read(uint8_t) noexcept override { return Sht31ReadResult{}; }
};
class TargetHeaterRail final : public IMq7HeaterRail {
 public:
  explicit TargetHeaterRail(uint8_t) noexcept {}
  bool select_high() noexcept override { return true; }
  bool select_low() noexcept override { return true; }
};
#endif

// Owns every target adapter and composes the complete graph. Profiles remain
// injected so an unprovisioned/config-required boot can keep the graph off.
class TargetAcquisition final {
 public:
  static constexpr std::size_t kHandoffCapacity{TargetAcquisitionGraph::kHandoffCapacity};

  TargetAcquisition(RecordFactory& factory, const board::BoardPins& pins,
                    const TargetProfileSet& profiles,
                    SampleRing<kHandoffCapacity>& handoff) noexcept
      : pins_(pins),
        hx_org_(pins_.organic_hx711_dout, pins_.organic_hx711_sck),
        hx_inorganic_(pins_.inorganic_hx711_dout, pins_.inorganic_hx711_sck),
        hx_b3_(pins_.b3_hx711_dout, pins_.b3_hx711_sck),
        us_org_(pins_.organic_ultrasonic_trig, pins_.organic_ultrasonic_echo),
        us_inorganic_(pins_.inorganic_ultrasonic_trig, pins_.inorganic_ultrasonic_echo),
        us_b3_(pins_.b3_ultrasonic_trig, pins_.b3_ultrasonic_echo),
        mq4_(pins_.mq4_adc), mq135_(pins_.mq135_adc), mq7_(pins_.mq7_adc), mq8_(pins_.mq8_adc),
        ds18b20_(pins_.ds18b20_data), sht31_(pins_.sht31_sda, pins_.sht31_scl),
        heater_rail_(pins_.mq7_heater_select),
        readers_{{&hx_org_, &hx_inorganic_, &hx_b3_},
                 {&us_org_, &us_inorganic_, &us_b3_},
                 {&mq4_, &mq135_, &mq7_, &mq8_}, &ds18b20_,
                 {&sht31_, &sht31_}, &heater_rail_},
        graph_(factory, readers_, profiles, handoff) {}

  bool begin(const config::SamplingConfig& sampling, uint64_t first_due_us) noexcept {
    return graph_.begin(sampling, first_due_us);
  }
  void disable() noexcept { graph_.disable(); }
  void poll(uint64_t now_us) noexcept { graph_.poll(now_us); }
  bool enabled() const noexcept { return graph_.enabled(); }
  std::size_t registered_jobs() const noexcept { return graph_.registered_jobs(); }
  SampleRing<kHandoffCapacity>& handoff() noexcept { return graph_.handoff(); }

 private:
  board::BoardPins pins_;
  TargetHx711Reader hx_org_;
  TargetHx711Reader hx_inorganic_;
  TargetHx711Reader hx_b3_;
  TargetUltrasonicReader us_org_;
  TargetUltrasonicReader us_inorganic_;
  TargetUltrasonicReader us_b3_;
  TargetMqAdcReader mq4_;
  TargetMqAdcReader mq135_;
  TargetMqAdcReader mq7_;
  TargetMqAdcReader mq8_;
  TargetDs18b20Reader ds18b20_;
  TargetSht31Reader sht31_;
  TargetHeaterRail heater_rail_;
  TargetReaderSet readers_{};
  TargetAcquisitionGraph graph_;
};

}  // namespace sigap::acquisition
