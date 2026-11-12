#include "runtime_debug_observer.h"

namespace sigap {
namespace {

bool sensor_metadata(acquisition::SensorId sensor, size_t& index,
                     int32_t& family) noexcept {
  using acquisition::SensorId;
  switch (sensor) {
    case SensorId::Mq4: index=0U; family=1; return true;
    case SensorId::Mq135: index=1U; family=1; return true;
    case SensorId::Mq7: index=2U; family=1; return true;
    case SensorId::Mq8: index=3U; family=1; return true;
    case SensorId::Hx711Organic: index=4U; family=2; return true;
    case SensorId::Hx711Inorganic: index=5U; family=2; return true;
    case SensorId::Hx711B3: index=6U; family=2; return true;
    case SensorId::UltrasonicOrganic: index=7U; family=3; return true;
    case SensorId::UltrasonicInorganic: index=8U; family=3; return true;
    case SensorId::UltrasonicB3: index=9U; family=3; return true;
    case SensorId::Ds18b20B3: index=10U; family=4; return true;
    case SensorId::Sht31_44: index=11U; family=4; return true;
    case SensorId::Sht31_45: index=12U; family=4; return true;
  }
  return false;
}

bool valid_quality(acquisition::SampleQuality quality,
                   int32_t& category) noexcept {
  using acquisition::SampleQuality;
  switch (quality) {
    case SampleQuality::Ready: category=0; return true;
    case SampleQuality::WarmingUp:
    case SampleQuality::HeaterPhase: category=1; return true;
    case SampleQuality::Stale: category=3; return true;
    case SampleQuality::Uncalibrated:
    case SampleQuality::Calibrating:
    case SampleQuality::Timeout:
    case SampleQuality::Disconnected:
    case SampleQuality::CrcError:
    case SampleQuality::OutOfRange: category=2; return true;
  }
  return false;
}

bool valid_reason(acquisition::SampleReason reason) noexcept {
  using acquisition::SampleReason;
  switch (reason) {
    case SampleReason::None:
    case SampleReason::DataReady:
    case SampleReason::NotReady:
    case SampleReason::Timeout:
    case SampleReason::Disconnected:
    case SampleReason::CrcError:
    case SampleReason::InvalidProfile:
    case SampleReason::HeaterPhase:
    case SampleReason::ConversionPending:
    case SampleReason::OutOfRange:
    case SampleReason::ProfileMissing: return true;
  }
  return false;
}

}  // namespace

RuntimeDebugObserver::RuntimeDebugObserver(debug::IEventSink& sink,
                                           acquisition::ISampleObserver& evidence) noexcept
    : sink_(sink), evidence_(evidence) {}

void RuntimeDebugObserver::observe(const acquisition::SampleRecord& sample) noexcept {
  evidence_.observe(sample);

  size_t index=0U;
  int32_t family=0;
  int32_t category=0;
  if (!sensor_metadata(sample.header.sensor,index,family) ||
      !valid_quality(sample.quality,category) || !valid_reason(sample.reason)) {
    return;
  }

  const auto quality=static_cast<uint8_t>(sample.quality);
  const auto reason=static_cast<uint8_t>(sample.reason);
  State& state=states_[index];
  if (state.present && state.quality==quality && state.reason==reason) return;
  state.present=true;
  state.quality=quality;
  state.reason=reason;

  debug::EventInput event{};
  event.monotonic_us=sample.header.monotonic_us;
  event.related_sequence=sample.header.sequence;
  event.arg0=category;
  event.arg1=family;
  event.event_code=40U;
  event.level=debug::Level::Info;
  event.module=debug::Module::Acquisition;
  event.presence=debug::kHasRelatedSequence;
  (void)sink_.emit(event);
}

}  // namespace sigap
