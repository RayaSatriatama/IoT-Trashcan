#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "sigap/build/build_config.h"
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
#include "acquisition/mq_adc_calibration_esp32.h"
#include "acquisition/sensors/ultrasonic.h"
#include "acquisition/sensors/sht31.h"
#include "acquisition/target_protocol.h"
#include "acquisition/simulated_readers.h"
#include "acquisition/inactive_readers.h"
#include "acquisition/usb_sensor_debug.h"
#include "debug/types.h"

#ifdef ARDUINO
#include "acquisition/ultrasonic_capture.h"
#include <Arduino.h>
#include "freertos/portmacro.h"
#include <Wire.h>
#include <esp_timer.h>
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
  // Capacity is a typed processing boundary. The pointer is populated by the
  // composition/profile layer when an active mechanical profile exists.
  std::array<const OperationalCapacityProfile*, 3> capacity{};
  const Ds18b20Profile* ds18b20{nullptr};
  std::array<const Sht31Profile*, 2> sht31{};
  const Mq7HeaterProfile* heater{nullptr};
};

class AcquisitionDebugCounters final {
 public:
  void bind_sink(debug::IEventSink* sink) noexcept { sink_ = sink; }
  void reset(uint64_t now_us) noexcept {
    produced_ = 0U;
    dropped_ = 0U;
    last_flush_us_ = now_us;
    last_poll_us_ = now_us;
  }
  void record(HandoffStatus status) noexcept {
    increment(produced_);
    if (status == HandoffStatus::Full) increment(dropped_);
  }
  void poll(uint64_t now_us) noexcept {
    const bool regressed = now_us < last_poll_us_;
    last_poll_us_ = now_us;
    if (regressed) {
      last_flush_us_ = now_us;
      return;
    }
    if (now_us - last_flush_us_ < 1000000U) return;
    last_flush_us_ = now_us;
    const auto produced = produced_;
    const auto dropped = dropped_;
    produced_ = 0U;
    dropped_ = 0U;
    emit(now_us, 0, produced);
    emit(now_us, 1, dropped);
  }

 private:
  static void increment(uint32_t& count) noexcept {
    if (count < static_cast<uint32_t>(INT32_MAX)) ++count;
  }
  void emit(uint64_t now_us, int32_t kind, uint32_t count) noexcept {
    if (sink_ == nullptr || count == 0U) return;
    debug::EventInput event{};
    event.monotonic_us = now_us;
    event.module = debug::Module::Acquisition;
    event.level = debug::Level::Debug;
    event.event_code = 41U;
    event.arg0 = kind;
    event.arg1 = static_cast<int32_t>(count);
    (void)sink_->emit(event);
  }
  debug::IEventSink* sink_{nullptr};
  uint32_t produced_{0U};
  uint32_t dropped_{0U};
  uint64_t last_flush_us_{0U};
  uint64_t last_poll_us_{0U};
};

template <typename Acquirer, std::size_t Capacity>
struct AcquisitionJob final {
  Acquirer* acquirer{nullptr};
  SampleRing<Capacity>* handoff{nullptr};
  SensorHealthState* health{nullptr};
  ISampleObserver* observer{nullptr};
  AcquisitionDebugCounters* counters{nullptr};
  uint64_t capture_period_us{0U};
  uint64_t next_capture_us{0U};

  static void run(void* context, uint64_t now_us) noexcept {
    auto* job = static_cast<AcquisitionJob*>(context);
    if (job == nullptr || job->acquirer == nullptr || job->handoff == nullptr) {
      return;
    }
    if (now_us < job->next_capture_us) return;
    SampleRecord record{};
    if (job->acquirer->poll(now_us, record) == DriverPollStatus::Produced) {
      if (job->capture_period_us != 0U) {
        const auto wait = job->capture_period_us - now_us % job->capture_period_us;
        job->next_capture_us = UINT64_MAX - now_us < wait ? UINT64_MAX : now_us + wait;
      }
      if (job->health != nullptr) {
        job->health->observe(now_us, record.quality);
      }
      if (job->observer != nullptr) job->observer->observe(record);
      const auto status = job->handoff->try_push(record);
      if (job->counters != nullptr) job->counters->record(status);
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

struct UltrasonicTriggerJob final {
  UltrasonicAcquirer* acquirer{nullptr};

  static void run(void* context, uint64_t now_us) noexcept {
    auto* job = static_cast<UltrasonicTriggerJob*>(context);
    if (job != nullptr && job->acquirer != nullptr) {
      (void)job->acquirer->request_sample(now_us);
    }
  }
};

class TargetAcquisitionGraph final {
 public:
  static constexpr std::size_t kHandoffCapacity{16};
  static constexpr std::size_t kJobCount{build::kPhysicalReferenceOnly ? 4U : 14U};

  TargetAcquisitionGraph(RecordFactory& factory,
                         const TargetReaderSet& readers,
                         const TargetProfileSet& profiles,
                         SampleRing<kHandoffCapacity>& handoff) noexcept
      : factory_(factory), readers_(readers), profiles_(profiles), handoff_(handoff) {}

  bool begin(const config::SamplingConfig& sampling, uint64_t first_due_us) noexcept {
    if (enabled_ || !all_readers_present()) {
      return false;
    }
    const uint64_t hx_period = build::kLiveNoBacklog ? build::kLiveSnapshotPeriodUs : milliseconds_to_us(sampling.hx711_period_ms);
    const uint64_t mq_period = build::kLiveNoBacklog ? build::kLiveSnapshotPeriodUs : milliseconds_to_us(sampling.mq_period_ms);
    const uint64_t ultrasonic_period = build::kLiveNoBacklog ? build::kLiveSnapshotPeriodUs : milliseconds_to_us(sampling.ultrasonic_period_ms);
    const uint64_t ds_period = milliseconds_to_us(sampling.ds18b20_period_ms);
    const uint64_t sht_period = build::kLiveNoBacklog ? 10000U : milliseconds_to_us(sampling.sht31_period_ms);
    if (hx_period == 0U || mq_period == 0U || ultrasonic_period == 0U ||
        ds_period == 0U || sht_period == 0U) {
      return false;
    }
    if (profiles_.ds18b20 == nullptr ||
        profiles_.ds18b20->manifest.validity == ProfileValidity::Missing) {
      ds_channel_state_ = DsChannelState::ProfileMissing;
    } else if (!profiles_.ds18b20->valid_for_acquisition() ||
               !profile_matches(profiles_.ds18b20->manifest,
                                SensorId::Ds18b20B3)) {
      ds_channel_state_ = DsChannelState::ProfileInvalid;
    } else if (!readers_.ds18b20->begin()) {
      ds_channel_state_ = DsChannelState::ProfileApplyFailed;
    } else {
      ds_channel_state_ = DsChannelState::Ready;
    }

    debug_counters_.reset(first_due_us);
    scheduler_ = CooperativeScheduler{};
    registered_jobs_ = 0U;
    health_ = {};
    us_gate_ = UltrasonicTriggerGate(
        effective_ultrasonic_gap(profiles_.ultrasonic));
    heater_profile_ = profiles_.heater != nullptr ? *profiles_.heater : Mq7HeaterProfile{};
    heater_.emplace(*readers_.heater_rail, heater_profile_);
    hx_org_.emplace(SensorId::Hx711Organic, StreamId::Organic, *readers_.hx711[0], factory_, profiles_.hx711[0], calibration_);
    hx_inorganic_.emplace(SensorId::Hx711Inorganic, StreamId::Inorganic, *readers_.hx711[1], factory_, profiles_.hx711[1], calibration_);
    hx_b3_.emplace(SensorId::Hx711B3, StreamId::B3, *readers_.hx711[2], factory_, profiles_.hx711[2], calibration_);
    us_org_.emplace(StreamId::Organic, *readers_.ultrasonic[0], factory_,
                    profiles_.ultrasonic[0],
                    resolve_ultrasonic_params(profiles_.ultrasonic[0],
                                              SensorId::UltrasonicOrganic),
                    us_gate_, calibration_);
    us_inorganic_.emplace(StreamId::Inorganic, *readers_.ultrasonic[1], factory_,
                          profiles_.ultrasonic[1],
                          resolve_ultrasonic_params(profiles_.ultrasonic[1],
                                                    SensorId::UltrasonicInorganic),
                          us_gate_, calibration_);
    us_b3_.emplace(StreamId::B3, *readers_.ultrasonic[2], factory_,
                   profiles_.ultrasonic[2],
                   resolve_ultrasonic_params(profiles_.ultrasonic[2],
                                             SensorId::UltrasonicB3),
                   us_gate_, calibration_);
    mq4_.emplace(SensorId::Mq4, StreamId::Organic, *readers_.mq_adc[0], factory_, profiles_.mq_adc[0], calibration_);
    mq135_.emplace(SensorId::Mq135, StreamId::Organic, *readers_.mq_adc[1], factory_, profiles_.mq_adc[1], calibration_);
    mq7_.emplace(SensorId::Mq7, StreamId::B3, *readers_.mq_adc[2], factory_, profiles_.mq_adc[2], &*heater_, calibration_);
    mq8_.emplace(SensorId::Mq8, StreamId::B3, *readers_.mq_adc[3], factory_, profiles_.mq_adc[3], calibration_);
    ds18b20_.emplace(*readers_.ds18b20, factory_, profiles_.ds18b20,
                     resolve_ds18b20_conversion_time(profiles_.ds18b20),
                     ds_channel_state_, calibration_);
    sht31_44_.emplace(0x44U, StreamId::Organic, *readers_.sht31[0], factory_, profiles_.sht31[0], calibration_);
    sht31_45_.emplace(0x45U, StreamId::B3, *readers_.sht31[1], factory_, profiles_.sht31[1], calibration_);
    hx_jobs_[0] = {&*hx_org_, &handoff_, &health_[0], observer_, &debug_counters_};
    hx_jobs_[1] = {&*hx_inorganic_, &handoff_, &health_[1], observer_, &debug_counters_};
    hx_jobs_[2] = {&*hx_b3_, &handoff_, &health_[2], observer_, &debug_counters_};
    us_jobs_[0] = {&*us_org_};
    us_jobs_[1] = {&*us_inorganic_};
    us_jobs_[2] = {&*us_b3_};
    mq_jobs_[0] = {&*mq4_, &handoff_, &health_[6], observer_, &debug_counters_};
    mq_jobs_[1] = {&*mq135_, &handoff_, &health_[7], observer_, &debug_counters_};
    mq_jobs_[2] = {&*mq7_, &handoff_, &health_[8], observer_, &debug_counters_};
    mq_jobs_[3] = {&*mq8_, &handoff_, &health_[9], observer_, &debug_counters_};
    heater_job_.controller = &*heater_;
    ds_job_ = {&*ds18b20_, &handoff_, &health_[10], observer_, &debug_counters_};
    sht_jobs_[0] = {&*sht31_44_, &handoff_, &health_[11], observer_, &debug_counters_};
    sht_jobs_[1] = {&*sht31_45_, &handoff_, &health_[12], observer_, &debug_counters_};
    if constexpr (build::kLiveNoBacklog) {
      // Complete a 15 ms conversion promptly; begin the next capture on the shared second.
      for (auto& job : sht_jobs_) job.capture_period_us = build::kLiveSnapshotPeriodUs;
    }

    bool ok = true;
    if constexpr (build::kPhysicalReferenceOnly) {
      ok = add(hx_period, &AcquisitionJob<Hx711Acquirer, kHandoffCapacity>::run, &hx_jobs_[0], first_due_us) && ok;
      ok = add(ultrasonic_period, &UltrasonicTriggerJob::run, &us_jobs_[0], first_due_us) && ok;
      ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[1], first_due_us) && ok;
      ok = add(sht_period, &AcquisitionJob<Sht31Acquirer, kHandoffCapacity>::run, &sht_jobs_[0], first_due_us) && ok;
      enabled_ = ok;
      return enabled_;
    }
    ok = add(hx_period, &AcquisitionJob<Hx711Acquirer, kHandoffCapacity>::run, &hx_jobs_[0], first_due_us) && ok;
    ok = add(hx_period, &AcquisitionJob<Hx711Acquirer, kHandoffCapacity>::run, &hx_jobs_[1], first_due_us) && ok;
    ok = add(hx_period, &AcquisitionJob<Hx711Acquirer, kHandoffCapacity>::run, &hx_jobs_[2], first_due_us) && ok;
    ok = add(ultrasonic_period, &UltrasonicTriggerJob::run, &us_jobs_[0], first_due_us) && ok;
    ok = add(ultrasonic_period, &UltrasonicTriggerJob::run, &us_jobs_[1], first_due_us) && ok;
    ok = add(ultrasonic_period, &UltrasonicTriggerJob::run, &us_jobs_[2], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[0], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[1], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[2], first_due_us) && ok;
    ok = add(mq_period, &AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>::run, &mq_jobs_[3], first_due_us) && ok;
    ok = add(ds_period, &AcquisitionJob<Ds18b20Acquirer, kHandoffCapacity>::run, &ds_job_, first_due_us) && ok;
    ok = add(sht_period, &AcquisitionJob<Sht31Acquirer, kHandoffCapacity>::run, &sht_jobs_[0], first_due_us) && ok;
    ok = add(sht_period, &AcquisitionJob<Sht31Acquirer, kHandoffCapacity>::run, &sht_jobs_[1], first_due_us) && ok;
    heater_->start(first_due_us);
    ok = add(mq_period, &HeaterJob::run, &heater_job_, first_due_us) && ok;
    enabled_ = ok;
    return enabled_;
  }

  void poll(uint64_t now_us) noexcept {
    if (enabled_) {
      service_ultrasonic(*us_org_, now_us, health_[3]);
      if constexpr (!build::kPhysicalReferenceOnly) {
        service_ultrasonic(*us_inorganic_, now_us, health_[4]);
        service_ultrasonic(*us_b3_, now_us, health_[5]);
      }
      scheduler_.poll(now_us);
      debug_counters_.poll(now_us);
    }
  }

  void disable() noexcept {
    enabled_ = false;
    debug_counters_.reset(0U);
  }

  void bind_debug_sink(debug::IEventSink* sink) noexcept {
    debug_counters_.bind_sink(sink);
  }

  void set_sample_observer(ISampleObserver* observer) noexcept {
    observer_ = observer;
  }

  void set_calibration_state_provider(
      const ICalibrationStateProvider* provider) noexcept {
    calibration_ = provider;
  }

  bool refresh_profiles(uint64_t now_us) noexcept {
    if (!enabled_) return false;
    if (!build::kPhysicalReferenceOnly && profiles_.heater != nullptr && heater_.has_value()) {
      heater_->set_profile(*profiles_.heater, now_us);
    }
    us_gate_.set_minimum_gap(effective_ultrasonic_gap(profiles_.ultrasonic));
    const bool profile_ready =
        profiles_.ds18b20 != nullptr &&
        profiles_.ds18b20->valid_for_acquisition() &&
        profile_matches(profiles_.ds18b20->manifest, SensorId::Ds18b20B3);
    if (!profile_ready) {
      ds_channel_state_ = profiles_.ds18b20 == nullptr
                              ? DsChannelState::ProfileMissing
                              : DsChannelState::ProfileInvalid;
      if (ds18b20_.has_value()) {
        ds18b20_->rebind_profile(profiles_.ds18b20, ds_channel_state_);
      }
      return true;
    }
    ds_channel_state_ = DsChannelState::Ready;
    if (ds18b20_.has_value()) {
      ds18b20_->rebind_profile(profiles_.ds18b20, ds_channel_state_);
    }
    return true;
  }

  bool enabled() const noexcept { return enabled_; }
  std::size_t registered_jobs() const noexcept { return registered_jobs_; }
  SampleRing<kHandoffCapacity>& handoff() noexcept { return handoff_; }
  const SampleRing<kHandoffCapacity>& handoff() const noexcept { return handoff_; }
  const Mq7HeaterController* heater() const noexcept {
    return heater_.has_value() ? &*heater_ : nullptr;
  }
  const SensorHealthState& health(std::size_t index) const noexcept {
    return health_[index < health_.size() ? index : 0U];
  }
  DsChannelState ds_channel_state() const noexcept { return ds_channel_state_; }

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
           std::all_of(readers_.sht31.begin(), readers_.sht31.end(), present) &&
           readers_.ds18b20 != nullptr && readers_.heater_rail != nullptr;
  }

  void service_ultrasonic(UltrasonicAcquirer& acquirer, uint64_t now_us,
                          SensorHealthState& health) noexcept {
    SampleRecord record{};
    if (acquirer.service(now_us, record) == DriverPollStatus::Produced) {
      health.observe(now_us, record.quality);
      if (observer_ != nullptr) observer_->observe(record);
      const auto status = handoff_.try_push(record);
      debug_counters_.record(status);
    }
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
  std::optional<Mq7HeaterController> heater_{};
  UltrasonicTriggerGate us_gate_{};
  std::optional<Hx711Acquirer> hx_org_{};
  std::optional<Hx711Acquirer> hx_inorganic_{};
  std::optional<Hx711Acquirer> hx_b3_{};
  std::optional<UltrasonicAcquirer> us_org_{};
  std::optional<UltrasonicAcquirer> us_inorganic_{};
  std::optional<UltrasonicAcquirer> us_b3_{};
  std::optional<MqAdcAcquirer> mq4_{};
  std::optional<MqAdcAcquirer> mq135_{};
  std::optional<MqAdcAcquirer> mq7_{};
  std::optional<MqAdcAcquirer> mq8_{};
  std::optional<Ds18b20Acquirer> ds18b20_{};
  std::optional<Sht31Acquirer> sht31_44_{};
  std::optional<Sht31Acquirer> sht31_45_{};
  std::array<AcquisitionJob<Hx711Acquirer, kHandoffCapacity>, 3> hx_jobs_{};
  std::array<UltrasonicTriggerJob, 3> us_jobs_{};
  std::array<AcquisitionJob<MqAdcAcquirer, kHandoffCapacity>, 4> mq_jobs_{};
  AcquisitionJob<Ds18b20Acquirer, kHandoffCapacity> ds_job_{};
  std::array<AcquisitionJob<Sht31Acquirer, kHandoffCapacity>, 2> sht_jobs_{};
  HeaterJob heater_job_{};
  std::array<SensorHealthState, 13> health_{};
  DsChannelState ds_channel_state_{DsChannelState::ProfileMissing};
  CooperativeScheduler scheduler_{};
  std::size_t registered_jobs_{0};
  bool enabled_{false};
  ISampleObserver* observer_{nullptr};
  const ICalibrationStateProvider* calibration_{nullptr};
  AcquisitionDebugCounters debug_counters_{};
};

// Minimal target GPIO adapters. They intentionally expose only bounded,
// non-blocking seams. Sensor libraries can replace these adapters later
// without changing graph ownership or scheduling.
#ifdef ARDUINO
class TargetMqAdcReader final : public IMqAdcReader {
 public:
  explicit TargetMqAdcReader(uint8_t pin) noexcept : pin_(pin) { pinMode(pin_, INPUT); }
  // Opt-in same-sample calibrated voltage evidence. Enabled only for the
  // MQ-135 channel by TargetAcquisition below; every other MQ channel keeps
  // exactly the previous behaviour.
  void enable_calibrated_voltage() noexcept { calibrated_ = true; }
  MqAdcReadResult read() noexcept override {
    // Per-pin attenuation BEFORE the sample it annotates. Only the enabled
    // channel is configured; every other MQ channel keeps the SDK default and
    // its existing single-read behaviour.
    if (calibrated_) mq_adc_configure_attenuation_for_pin(pin_);
    const int value = analogRead(pin_);
    if (value < 0) return MqAdcReadResult{false, true, 0U, SampleReason::Disconnected};
    MqAdcReadResult result{true, false, static_cast<uint16_t>(value), SampleReason::DataReady};
    // One analogRead() only: the calibrated voltage is derived from this exact
    // raw sample, never from a second conversion.
    if (calibrated_) result.calibrated = mq_adc_calibrated_voltage_for_pin(pin_, result.counts);
    return result;
  }

 private:
  uint8_t pin_;
  bool calibrated_{false};
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
    // Critical section: disable interrupts to prevent HIGH pulse extension
    // beyond HX711 PD_SCK max 50us / power-down >60us (AVIA datasheet p5)
    portENTER_CRITICAL_ISR(&hx711_spinlock_);
    for (uint8_t bit = 0; bit < 24U; ++bit) {
      digitalWrite(sck_, HIGH);
      // NOP loop for ~1us instead of delayMicroseconds(1) which can be interrupted
      for (volatile uint32_t i = 0; i < 160; ++i) { asm volatile("nop"); }
      value = (value << 1U) | (digitalRead(dout_) == HIGH ? 1U : 0U);
      digitalWrite(sck_, LOW);
      for (volatile uint32_t i = 0; i < 160; ++i) { asm volatile("nop"); }
    }
    // 25th pulse for gain 128 (channel A)
    digitalWrite(sck_, HIGH);
    for (volatile uint32_t i = 0; i < 160; ++i) { asm volatile("nop"); }
    digitalWrite(sck_, LOW);
    // Ensure SCK stays LOW on return (power-down protection)
    portEXIT_CRITICAL_ISR(&hx711_spinlock_);
    const int64_t signed_value = (value & 0x800000U) != 0U
                                      ? static_cast<int64_t>(value) - 0x1000000LL
                                      : static_cast<int64_t>(value);
    return Hx711ReadResult{true, false, signed_value, 1U, 0U, SampleReason::DataReady};
  }

 private:
  uint8_t dout_;
  uint8_t sck_;
  static inline portMUX_TYPE hx711_spinlock_ = portMUX_INITIALIZER_UNLOCKED;
};

struct UsbSensorDebugStats { uint32_t triggers{}, polls{}, cancels{}, ready{}, high{}, last_width{}; uint64_t max_gap_us{}, last_poll_us{}; };

class TargetUltrasonicReader final : public IUltrasonicReader {
 public:
  TargetUltrasonicReader(uint8_t trigger, uint8_t echo) noexcept
      : trigger_(trigger), echo_(echo) {
    pinMode(trigger_, OUTPUT);
    pinMode(echo_, INPUT);
    digitalWrite(trigger_, LOW);
  }
  bool trigger(uint64_t now_us) noexcept override {
    (void)now_us;
    ++debug_.triggers;
    if (active_) return false;
    active_ = capture_.trigger(trigger_, echo_, timeout_us_);
    return active_;
  }
  UltrasonicEchoResult poll(uint64_t now_us) noexcept override {
    ++debug_.polls;
    const uint64_t actual_us = static_cast<uint64_t>(esp_timer_get_time());
    if (debug_.last_poll_us && actual_us > debug_.last_poll_us && actual_us-debug_.last_poll_us > debug_.max_gap_us) debug_.max_gap_us=actual_us-debug_.last_poll_us;
    debug_.last_poll_us=actual_us;
    if (!active_) return UltrasonicEchoResult{UltrasonicEchoStatus::Pending, 0U};
    (void)now_us;
    if (digitalRead(echo_) == HIGH) ++debug_.high;
    const auto result = capture_.poll();
    if (result.status == UltrasonicEchoStatus::Ready) {
      ++debug_.ready;
      debug_.last_width = result.echo_us;
    }
    if (result.status != UltrasonicEchoStatus::Pending) active_ = false;
    return result;
  }

  const UsbSensorDebugStats& debug_stats() const noexcept { return debug_; }
  void cancel() noexcept override {
    ++debug_.cancels;
    active_ = false;
    capture_.cancel();
    capture_.emit_debug();
  }
  void set_echo_timeout(uint32_t timeout_us) noexcept override { timeout_us_ = timeout_us; }
  bool hardware_latched() const noexcept override { return true; }
  uint64_t trigger_time_us(uint64_t) const noexcept override { return capture_.trigger_time_us(); }
  uint32_t readout_grace_us() const noexcept override { return capture_.readout_grace_us(); }

 private:
  uint8_t trigger_;
  uint8_t echo_;
  bool active_{false};
  uint32_t timeout_us_{30000U};
  UltrasonicCapture capture_{};
  UsbSensorDebugStats debug_{};
};

class TargetDs18b20Reader final : public IDs18b20Reader {
 public:
  TargetDs18b20Reader(uint8_t data_pin,
                      const Ds18b20Profile* profile = nullptr) noexcept
      : data_pin_(data_pin) {
    pinMode(data_pin_, INPUT_PULLUP);
    set_profile(profile);
  }

  void set_profile(const Ds18b20Profile* profile) noexcept {
    profile_ = profile;
    expected_rom_ = {};
    expected_rom_valid_ = false;
    configured_ = false;
    conversion_requested_ = false;
    completion_observed_ = false;
    discovery_mode_ = false;
    if (profile_ != nullptr && profile_->valid_for_acquisition() &&
        profile_matches(profile_->manifest, SensorId::Ds18b20B3) &&
        profile_->rom[0] == 0x28U &&
        ds18b20_crc8(profile_->rom.data(), 7U) == profile_->rom[7]) {
      expected_rom_ = profile_->rom;
      expected_rom_valid_ = true;
    }
  }

  bool begin() noexcept override {
    if (!expected_rom_valid_ || profile_ == nullptr ||
        !profile_->valid_for_acquisition()) {
      return false;
    }

    if (!verify_rom_identity()) return false;

    Ds18b20Scratchpad scratchpad{};
    if (!read_scratchpad(scratchpad) || !scratchpad.crc_valid) {
      return false;
    }
    if (!ds18b20_resolution_matches(scratchpad, profile_->resolution_bits)) {
      if (!write_scratchpad(scratchpad.th, scratchpad.tl,
                            ds18b20_resolution_config(
                                profile_->resolution_bits))) {
        return false;
      }
      Ds18b20Scratchpad verify{};
      if (!read_scratchpad(verify) ||
          !ds18b20_resolution_matches(verify, profile_->resolution_bits)) {
        return false;
      }
    }
    configured_ = true;
    return true;
  }

  bool apply_profile(const Ds18b20Profile* profile) noexcept override {
    set_profile(profile);
    return begin();
  }

  Ds18b20DiscoveryStatus discover() noexcept override {
    // Discovery is deliberately separate from begin(): begin() is the
    // strict profile-bound path, while this path is enabled only by an
    // explicit calibration owner in Ds18b20Acquirer.
    if (profile_ != nullptr) {
      return Ds18b20DiscoveryStatus::Unavailable;
    }
    if (!reset_presence()) {
      return Ds18b20DiscoveryStatus::Disconnected;
    }

    // This data pin is a dedicated single-device bus. READ ROM is therefore
    // safe and keeps the bounded adapter independent from a stored profile.
    write_byte(0x33U);
    std::array<uint8_t, 8> rom{};
    for (uint8_t& byte : rom) byte = read_byte();
    if (rom[0] != 0x28U || ds18b20_crc8(rom.data(), 7U) != rom[7]) {
      expected_rom_ = {};
      expected_rom_valid_ = false;
      configured_ = false;
      discovery_mode_ = false;
      return Ds18b20DiscoveryStatus::InvalidRom;
    }

    expected_rom_ = rom;
    last_rom_ = rom;
    expected_rom_valid_ = true;
    configured_ = true;
    discovery_mode_ = true;
    conversion_requested_ = false;
    completion_observed_ = false;
    return Ds18b20DiscoveryStatus::Ready;
  }

  bool request_conversion() noexcept override {
    if (!configured_ || !expected_rom_valid_ || conversion_requested_ ||
        !reset_presence()) {
      return false;
    }
    write_byte(0x55U);  // MATCH ROM
    for (const uint8_t byte : expected_rom_) write_byte(byte);
    write_byte(0x44U);  // CONVERT T, return immediately
    conversion_requested_ = true;
    return true;
  }

  bool conversion_ready() noexcept override {
    if (!conversion_requested_) return false;
    const bool ready = read_bit();
    if (ready) {
      completion_observed_ = true;
    } else {
      // The acquirer has reached its profile-derived deadline. Drop this
      // request so the next scheduled attempt starts a fresh conversion.
      conversion_requested_ = false;
    }
    return ready;
  }

  Ds18b20ReadResult read_conversion() noexcept override {
    Ds18b20ReadResult result{};
    if (!conversion_requested_ || !completion_observed_) return result;
    conversion_requested_ = false;
    completion_observed_ = false;
    Ds18b20Scratchpad scratchpad{};
    result.present = read_scratchpad(scratchpad);
    if (!result.present) return result;
    result.crc_valid = scratchpad.crc_valid;
    result.identity_valid = verify_rom_identity();
    result.resolution_matches =
        profile_ != nullptr
            ? ds18b20_resolution_matches(scratchpad,
                                         profile_->resolution_bits)
            : (discovery_mode_ &&
               ds18b20_resolution_from_config(scratchpad.config) != 0U);
    result.temperature_centi_c = static_cast<int32_t>(scratchpad.raw_temperature) *
                                 625 / 100;
    result.conversion_complete = true;
    if (result.identity_valid) {
      result.wire_evidence = true;
      result.rom = last_rom_;
      result.scratchpad = last_scratchpad_;
      result.resolution_bits = ds18b20_resolution_from_config(scratchpad.config);
    }
    return result;
  }

 private:
  bool read_scratchpad(Ds18b20Scratchpad& decoded) noexcept {
    if (!expected_rom_valid_ || !reset_presence()) return false;

    write_byte(0x55U);  // MATCH ROM
    for (const uint8_t byte : expected_rom_) write_byte(byte);
    write_byte(0xbeU);  // READ SCRATCHPAD
    std::array<uint8_t, 9> scratchpad{};
    for (uint8_t& byte : scratchpad) byte = read_byte();
    last_scratchpad_ = scratchpad;
    (void)parse_ds18b20_scratchpad(scratchpad, decoded);
    return true;
  }

  bool verify_rom_identity() noexcept {
    if (!expected_rom_valid_ || !reset_presence()) return false;

    write_byte(0x33U);  // READ ROM
    std::array<uint8_t, 8> rom{};
    for (uint8_t& byte : rom) byte = read_byte();
    last_rom_ = rom;
    return ds18b20_crc8(rom.data(), 7U) == rom[7] && rom == expected_rom_;
  }

  bool write_scratchpad(uint8_t th, uint8_t tl, uint8_t config) noexcept {
    if (!expected_rom_valid_ || !reset_presence()) return false;

    write_byte(0x55U);  // MATCH ROM
    for (const uint8_t byte : expected_rom_) write_byte(byte);
    write_byte(0x4eU);  // WRITE SCRATCHPAD; do not COPY to EEPROM.
    write_byte(th);
    write_byte(tl);
    write_byte(config);
    return true;
  }

  bool reset_presence() noexcept {
    pinMode(data_pin_, OUTPUT);
    digitalWrite(data_pin_, LOW);
    delayMicroseconds(480U);
    pinMode(data_pin_, INPUT_PULLUP);
    delayMicroseconds(70U);
    const bool present = digitalRead(data_pin_) == LOW;
    delayMicroseconds(410U);
    return present;
  }

  void write_bit(bool value) noexcept {
    pinMode(data_pin_, OUTPUT);
    digitalWrite(data_pin_, LOW);
    if (value) {
      delayMicroseconds(6U);
      pinMode(data_pin_, INPUT_PULLUP);
      delayMicroseconds(64U);
    } else {
      delayMicroseconds(60U);
      pinMode(data_pin_, INPUT_PULLUP);
      delayMicroseconds(10U);
    }
  }

  bool read_bit() noexcept {
    pinMode(data_pin_, OUTPUT);
    digitalWrite(data_pin_, LOW);
    delayMicroseconds(3U);
    pinMode(data_pin_, INPUT_PULLUP);
    delayMicroseconds(10U);
    const bool value = digitalRead(data_pin_) == HIGH;
    delayMicroseconds(50U);
    return value;
  }

  void write_byte(uint8_t value) noexcept {
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      write_bit((value & (1U << bit)) != 0U);
    }
  }

  uint8_t read_byte() noexcept {
    uint8_t value{0U};
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      if (read_bit()) value = static_cast<uint8_t>(value | (1U << bit));
    }
    return value;
  }

  uint8_t data_pin_;
  const Ds18b20Profile* profile_{nullptr};
  std::array<uint8_t, 8> expected_rom_{};
  std::array<uint8_t, 8> last_rom_{};
  std::array<uint8_t, 9> last_scratchpad_{};
  bool expected_rom_valid_{false};
  bool configured_{false};
  bool conversion_requested_{false};
  bool completion_observed_{false};
  bool discovery_mode_{false};
};

class TargetSht31Reader final : public ISht31Reader {
 public:
  TargetSht31Reader(uint8_t sda, uint8_t scl) noexcept : sda_(sda), scl_(scl) {
    initialized_ = Wire.begin(sda_, scl_);
    debug_.initialized = initialized_;
    Wire.setTimeOut(20U);
  }

  Sht31ReadResult read(uint8_t address) noexcept override {
    Sht31ReadResult result{};
    const int index = address == 0x44U ? 0 : (address == 0x45U ? 1 : -1);
    if (!initialized_ || index < 0) return result;

    ++debug_.calls[index].calls;
    const uint64_t now_us = monotonic_now_us();
    if (!pending_[static_cast<std::size_t>(index)]) {
      Wire.beginTransmission(address);
      Wire.write(0x24U);  // single-shot, high repeatability, no clock stretch
      Wire.write(0x00U);
      debug_.end_code[index] = Wire.endTransmission(true);
      if (debug_.end_code[index] != 0U) return result;
      pending_[static_cast<std::size_t>(index)] = true;
      ready_at_us_[static_cast<std::size_t>(index)] = now_us + 15000U;
      result.pending = true;
      return result;
    }
    if (now_us < ready_at_us_[static_cast<std::size_t>(index)]) {
      result.pending = true;
      return result;
    }

    pending_[static_cast<std::size_t>(index)] = false;
    debug_.bytes[index] = Wire.requestFrom(static_cast<uint8_t>(address), static_cast<std::size_t>(6U), true);
    if (debug_.bytes[index] != 6U) return result;
    std::array<uint8_t, 6> frame{};
    for (uint8_t& byte : frame) {
      if (Wire.available() <= 0) return Sht31ReadResult{};
      byte = static_cast<uint8_t>(Wire.read());
    }
    const auto decoded = decode_sht31_frame(frame);
    // A complete frame is present even when one CRC fails. Preserve the
    // decoded bytes as diagnostic evidence, while the acquirer suppresses
    // operational conversion and reports CrcError.
    ++debug_.completed[index];
    debug_.temp_crc[index]=decoded.temperature_crc_valid; debug_.hum_crc[index]=decoded.humidity_crc_valid;
    result.ok = true;
    result.crc_valid = decoded.temperature_crc_valid && decoded.humidity_crc_valid;
    result.temperature_crc_valid = decoded.temperature_crc_valid;
    result.humidity_crc_valid = decoded.humidity_crc_valid;
    result.temperature_centi_c = decoded.temperature_centi_c;
    result.humidity_centi_pct = decoded.humidity_centi_pct;
    result.temperature_ticks = static_cast<uint16_t>(
        (static_cast<uint16_t>(frame[0]) << 8U) | frame[1]);
    result.humidity_ticks = static_cast<uint16_t>(
        (static_cast<uint16_t>(frame[3]) << 8U) | frame[4]);
    return result;
  }

 public:
  struct Stats { bool initialized{}; struct One { uint32_t calls{}; }; One calls[2]{}; uint32_t completed[2]{}; uint8_t end_code[2]{}, bytes[2]{}; bool temp_crc[2]{}, hum_crc[2]{}; };
  const Stats& debug_stats() const noexcept { return debug_; }
 private:
  static uint64_t monotonic_now_us() noexcept {
    const int64_t value = esp_timer_get_time();
    return value < 0 ? 0U : static_cast<uint64_t>(value);
  }

  uint8_t sda_;
  uint8_t scl_;
  bool initialized_{false};
  std::array<bool, 2> pending_{};
  std::array<uint64_t, 2> ready_at_us_{};
  Stats debug_{};
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
  void enable_calibrated_voltage() noexcept {}
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
  UltrasonicEchoResult poll(uint64_t) noexcept override { return {}; }
  void cancel() noexcept override {}
};
class TargetDs18b20Reader final : public IDs18b20Reader {
 public:
  explicit TargetDs18b20Reader(uint8_t, const Ds18b20Profile* = nullptr) noexcept {}
  void set_profile(const Ds18b20Profile*) noexcept {}
  Ds18b20DiscoveryStatus discover() noexcept override {
    return Ds18b20DiscoveryStatus::Unavailable;
  }
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
using SelectedHx711Reader = std::conditional_t<build::kSensorSimulation, SimulatedHx711Reader, TargetHx711Reader>;
using SelectedUltrasonicReader = std::conditional_t<build::kSensorSimulation, SimulatedUltrasonicReader, TargetUltrasonicReader>;
using SelectedMqAdcReader = std::conditional_t<build::kSensorSimulation, SimulatedMqAdcReader, TargetMqAdcReader>;
using SelectedDs18b20Reader = std::conditional_t<build::kPhysicalReferenceOnly, InactiveDs18b20Reader, std::conditional_t<build::kSensorSimulation, SimulatedDs18b20Reader, TargetDs18b20Reader>>;
using SelectedSht31Reader = std::conditional_t<build::kSensorSimulation, SimulatedSht31Reader, TargetSht31Reader>;
using SelectedHeaterRail = std::conditional_t<build::kPhysicalReferenceOnly, InactiveHeaterRail, std::conditional_t<build::kSensorSimulation, SimulatedHeaterRail, TargetHeaterRail>>;
using SecondaryHx711Reader = std::conditional_t<build::kPhysicalReferenceOnly, InactiveHx711Reader, SelectedHx711Reader>;
using SecondaryUltrasonicReader = std::conditional_t<build::kPhysicalReferenceOnly, InactiveUltrasonicReader, SelectedUltrasonicReader>;
using SecondaryMqAdcReader = std::conditional_t<build::kPhysicalReferenceOnly, InactiveMqAdcReader, SelectedMqAdcReader>;

// Same-sample calibrated ADC voltage evidence is enabled for exactly one
// channel: the existing MQ-135 ADC channel (board::BoardPins::mq135_adc). No pin
// is reassigned and simulated / inactive readers stay evidence-free.
template <typename Reader>
inline void enable_mq135_calibrated_voltage(Reader& reader) noexcept {
  if constexpr (std::is_same_v<Reader, TargetMqAdcReader>) {
    reader.enable_calibrated_voltage();
  } else {
    (void)reader;
  }
}

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
        ds18b20_(pins_.ds18b20_data, nullptr), sht31_(pins_.sht31_sda, pins.sht31_scl),
        heater_rail_(pins_.mq7_heater_select),
        profiles_(profiles),
        readers_{{&hx_org_, &hx_inorganic_, &hx_b3_},
                 {&us_org_, &us_inorganic_, &us_b3_},
                 {&mq4_, &mq135_, &mq7_, &mq8_}, &ds18b20_,
                 {&sht31_, &sht31_}, &heater_rail_},
        graph_(factory, readers_, profiles, handoff) {
    // Only the actual MQ-135 channel gets calibrated-voltage evidence.
    enable_mq135_calibrated_voltage(mq135_);
  }

  bool begin(const config::SamplingConfig& sampling, uint64_t first_due_us) noexcept {
#if SIGAP_SENSOR_SIMULATION
    hx_org_.reset(); hx_inorganic_.reset(); hx_b3_.reset();
    us_org_.reset(); us_inorganic_.reset(); us_b3_.reset();
    mq4_.reset(); mq135_.reset(); mq7_.reset(); mq8_.reset(); sht31_.reset();
    ds18b20_.set_now(first_due_us); sht31_.set_now(first_due_us);
#endif
    ds18b20_.set_profile(profiles_.ds18b20);
    return graph_.begin(sampling, first_due_us);
  }
  void disable() noexcept { graph_.disable(); }
  void poll(uint64_t now_us) noexcept {
#if SIGAP_SENSOR_SIMULATION
    ds18b20_.set_now(now_us); sht31_.set_now(now_us);
#endif
    graph_.poll(now_us);
  }
  void emit_usb_debug(uint64_t actual_us) const noexcept {
#if defined(ARDUINO) && !SIGAP_SENSOR_SIMULATION
    static uint64_t last_emit_us{0U};
    if (actual_us < last_emit_us) last_emit_us = actual_us;
    if (actual_us - last_emit_us < 1000000U) return;
    last_emit_us = actual_us;
    const auto& sht = sht31_.debug_stats();
    const auto& ultrasonic = us_org_.debug_stats();
    emit_usb_sensor_debug(sht.initialized, sht.calls[0].calls, sht.completed[0],
                          sht.end_code[0], sht.bytes[0], sht.temp_crc[0],
                          sht.hum_crc[0], ultrasonic.triggers, ultrasonic.polls,
                          ultrasonic.high, ultrasonic.ready, ultrasonic.last_width,
                          ultrasonic.max_gap_us);
#else
    (void)actual_us;
#endif
  }
  bool refresh_profiles(uint64_t now_us) noexcept {
    return graph_.refresh_profiles(now_us);
  }
  void set_sample_observer(ISampleObserver* observer) noexcept {
    graph_.set_sample_observer(observer);
  }
  void bind_debug_sink(debug::IEventSink* sink) noexcept {
    graph_.bind_debug_sink(sink);
  }
  void set_calibration_state_provider(
      const ICalibrationStateProvider* provider) noexcept {
    graph_.set_calibration_state_provider(provider);
  }
  bool enabled() const noexcept { return graph_.enabled(); }
  std::size_t registered_jobs() const noexcept { return graph_.registered_jobs(); }
  SampleRing<kHandoffCapacity>& handoff() noexcept { return graph_.handoff(); }

 private:
  board::BoardPins pins_;
  SelectedHx711Reader hx_org_;
  SecondaryHx711Reader hx_inorganic_;
  SecondaryHx711Reader hx_b3_;
  SelectedUltrasonicReader us_org_;
  SecondaryUltrasonicReader us_inorganic_;
  SecondaryUltrasonicReader us_b3_;
  SecondaryMqAdcReader mq4_;
  SelectedMqAdcReader mq135_;
  SecondaryMqAdcReader mq7_;
  SecondaryMqAdcReader mq8_;
  SelectedDs18b20Reader ds18b20_;
  SelectedSht31Reader sht31_;
  SelectedHeaterRail heater_rail_;
  const TargetProfileSet& profiles_;
  TargetReaderSet readers_{};
  TargetAcquisitionGraph graph_;
};

}  // namespace sigap::acquisition
