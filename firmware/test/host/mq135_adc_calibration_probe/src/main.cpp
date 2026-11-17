// S1 build-only probe for the same-sample calibrated ADC seam.
//
// This is NOT flashed by this slice and it performs no hardware validation
// claim: it exists so the real ESP32-S3 SDK path (esp_adc_cal EFUSE_TP_FIT probe
// -> characterize -> raw_to_voltage on the same analogRead sample) is compiled
// and can later be inspected on a device by the owner.
//
// Build: /home/raya/.platformio/penv/bin/pio run -d firmware/test/host/mq135_adc_calibration_probe
#include <Arduino.h>

#include "acquisition/arduino_adapters.h"
#include "acquisition/mq_adc_calibration_esp32.h"

using namespace sigap::acquisition;

namespace {

// Unchanged board wiring: board::BoardPins::mq135_adc == 5 -> ADC1 channel 4.
constexpr uint8_t kMq135Pin = 5U;

const char* method_name(MqAdcCalibrationMethod method) {
  switch (method) {
    case MqAdcCalibrationMethod::None: return "none";
    case MqAdcCalibrationMethod::EspAdcCalEfuseTpFit: return "efuse_tp_fit";
  }
  return "?";
}

const char* failure_name(MqAdcCalibrationFailure failure) {
  switch (failure) {
    case MqAdcCalibrationFailure::None: return "none";
    case MqAdcCalibrationFailure::NotAttempted: return "not_attempted";
    case MqAdcCalibrationFailure::EfuseUnsupported: return "efuse_unsupported";
    case MqAdcCalibrationFailure::CharacterizeUnsupported: return "characterize_unsupported";
    case MqAdcCalibrationFailure::ConversionFailed: return "conversion_failed";
    case MqAdcCalibrationFailure::RawOutOfRange: return "raw_out_of_range";
    case MqAdcCalibrationFailure::ImplausibleVoltage: return "implausible_voltage";
  }
  return "?";
}

void report(const char* label, const MqAdcReadResult& result) {
  const MqAdcCalibratedVoltage& evidence = result.calibrated;
  Serial.printf(
      "%s ok=%d counts=%u cal=%d method=%s failure=%s unit=%u res=%u atten=%u "
      "adc_unit=%u adc_ch=%u mv=%.2f same_sample=%d\n",
      label, result.ok ? 1 : 0, static_cast<unsigned>(result.counts),
      evidence.available ? 1 : 0, method_name(evidence.method),
      failure_name(evidence.failure), static_cast<unsigned>(evidence.unit),
      static_cast<unsigned>(evidence.resolution_bits),
      static_cast<unsigned>(evidence.attenuation_db),
      static_cast<unsigned>(evidence.adc_unit),
      static_cast<unsigned>(evidence.adc_channel),
      static_cast<double>(evidence.pin_mv), evidence.same_sample ? 1 : 0);
}

// Availability semantics: an available record must be a same-sample mV value for
// these exact counts; an unavailable record must be an explicit failure with no
// voltage - never 0 mV presented as a measurement and never 3300 mV.
bool consistent(const MqAdcReadResult& result) {
  if (!result.ok) return true;
  const MqAdcCalibratedVoltage& evidence = result.calibrated;
  if (evidence.available) return mq_adc_calibration_matches(evidence, result.counts);
  return evidence.pin_mv == 0.0F && evidence.same_sample == false &&
         evidence.method == MqAdcCalibrationMethod::None &&
         evidence.unit == MqAdcVoltageUnit::None &&
         evidence.failure != MqAdcCalibrationFailure::None;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  const MqAdcChannelMap channel = mq_adc_channel_map(kMq135Pin);
  Serial.printf("PROBE pin=%u adc_unit=%u adc_channel=%u resolved=%d res=%u atten=%u\n",
                static_cast<unsigned>(kMq135Pin), static_cast<unsigned>(channel.unit),
                static_cast<unsigned>(channel.channel), channel.resolved ? 1 : 0,
                static_cast<unsigned>(kMqAdcEffectiveResolutionBits),
                static_cast<unsigned>(kMqAdcEffectiveAttenuationDb));

  ArduinoMqAdcReader calibrated(kMq135Pin);
  calibrated.enable_calibrated_voltage();
  ArduinoMqAdcReader plain(kMq135Pin);  // evidence must stay disabled here

  bool ok = channel.resolved && channel.unit == 1U && channel.channel == 4U;
  for (int index = 0; index < 3; ++index) {
    const MqAdcReadResult with_evidence = calibrated.read();
    report("calibrated", with_evidence);
    ok = ok && consistent(with_evidence);
    const MqAdcReadResult without_evidence = plain.read();
    report("plain     ", without_evidence);
    ok = ok && !without_evidence.calibrated.available &&
         without_evidence.calibrated.failure == MqAdcCalibrationFailure::NotAttempted;
  }
  Serial.println(ok ? "PROBE_OK" : "PROBE_FAIL");
}

void loop() { delay(1000); }
