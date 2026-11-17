// S1 B1 behavioural regression: the MQ-135 per-pin attenuation must be in
// effect BEFORE the single analogRead() whose counts the calibrated evidence
// annotates, in both real target adapters.
//
// The adapters (acquisition/arduino_adapters.h, acquisition/target_runtime.h)
// and the esp_adc_cal seam (acquisition/mq_adc_calibration_esp32.h) are the
// real firmware headers; only the SDK surface is stubbed (include/Arduino.h,
// include/esp_adc_cal.h). The stub starts at 6 dB, not at the 11 dB SDK
// default, so a post-read configuration cannot pass by accident.
//
// Exit code 0 = every case passed, 1 = at least one case failed.

#include <cstdio>

#include "acquisition/arduino_adapters.h"
#include "acquisition/target_runtime.h"

using namespace sigap::acquisition;

namespace {

constexpr uint8_t kMq135Pin{5U};
constexpr uint16_t kSampleCounts{2700U};
constexpr int kExpectedUnit{1};
constexpr int kExpectedChannel{4};

int failures{0};

void check(bool condition, const char* test, const char* message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL %s: %s\n", test, message);
  }
}

void check_equal_int(int actual, int expected, const char* test, const char* message) {
  if (actual != expected) {
    ++failures;
    std::printf("FAIL %s: %s (expected %d, got %d)\n", test, message, expected, actual);
  }
}

// The corrected sequence, asserted on the SDK call trace and on the evidence.
template <typename Reader>
void check_corrected_sequence(const char* test, Reader& reader) {
  adc_stub::reset();
  const MqAdcReadResult result = reader.read();

  const adc_stub::Call* configuration =
      adc_stub::first(adc_stub::CallKind::ConfigureAttenuation);
  const adc_stub::Call* sample = adc_stub::first(adc_stub::CallKind::Read);
  const adc_stub::Call* conversion = adc_stub::first(adc_stub::CallKind::Convert);

  check(configuration != nullptr, test, "per-pin attenuation is configured");
  check(sample != nullptr, test, "the channel is sampled");
  if (configuration == nullptr || sample == nullptr) return;

  check(configuration < sample, test,
        "attenuation is configured BEFORE the single analogRead");
  check_equal_int(adc_stub::count(adc_stub::CallKind::ConfigureAttenuation), 1, test,
                  "exactly one attenuation configuration");
  check_equal_int(adc_stub::count(adc_stub::CallKind::Read), 1, test,
                  "exactly one analogRead");
  check_equal_int(adc_stub::count(adc_stub::CallKind::ReadMilliVolts), 0, test,
                  "no second analogReadMilliVolts pass");
  check_equal_int(configuration->attenuation, static_cast<int>(ADC_11db), test,
                  "configuration requests ADC_11db");
  check_equal_int(configuration->pin, kMq135Pin, test,
                  "configuration targets the MQ-135 pin");
  check_equal_int(sample->attenuation, static_cast<int>(ADC_11db), test,
                  "the sample is taken at 11 dB");

  check(result.ok, test, "the read succeeds");
  check(!result.disconnected, test, "the read is not disconnected");
  check_equal_int(result.counts, kSampleCounts, test, "raw counts are preserved");
  check(result.calibrated.available, test, "calibrated evidence is available");
  check(result.calibrated.same_sample, test, "evidence claims the same sample");
  check_equal_int(result.calibrated.counts, result.counts, test,
                  "evidence carries the same raw counts");
  check_equal_int(result.calibrated.attenuation_db, 11, test,
                  "evidence reports the configured attenuation");
  check_equal_int(result.calibrated.resolution_bits, 12, test,
                  "evidence reports 12-bit resolution");
  check_equal_int(result.calibrated.adc_unit, kExpectedUnit, test, "evidence reports ADC1");
  check_equal_int(result.calibrated.adc_channel, kExpectedChannel, test,
                  "evidence reports ADC1 channel 4");

  check(conversion != nullptr, test, "the sample is converted");
  if (conversion != nullptr) {
    check_equal_int(conversion->counts, kSampleCounts, test,
                    "conversion uses exactly the read counts");
    check_equal_int(conversion->attenuation, static_cast<int>(ADC_ATTEN_DB_12), test,
                    "conversion uses the 11 dB characterization");
  }
  check(result.calibrated.pin_mv ==
            static_cast<float>(adc_stub::mv_for(kSampleCounts)),
        test, "pin_mv is derived from the converted counts");
}

// A channel that never opted in must keep the legacy single-read behaviour.
template <typename Reader>
void check_disabled_legacy(const char* test, Reader& reader) {
  adc_stub::reset();
  const MqAdcReadResult result = reader.read();

  check_equal_int(adc_stub::count(adc_stub::CallKind::Read), 1, test,
                  "legacy channel still performs exactly one analogRead");
  check_equal_int(adc_stub::count(adc_stub::CallKind::ConfigureAttenuation), 0, test,
                  "disabled channel configures no attenuation");
  check_equal_int(adc_stub::count(adc_stub::CallKind::Convert), 0, test,
                  "disabled channel performs no conversion");
  check_equal_int(adc_stub::count(adc_stub::CallKind::ReadMilliVolts), 0, test,
                  "disabled channel does not call analogReadMilliVolts");
  check(result.ok, test, "legacy read succeeds");
  check_equal_int(result.counts, kSampleCounts, test, "legacy raw counts are unchanged");
  check(!result.calibrated.available, test, "disabled channel carries no evidence");
  check(!result.calibrated.same_sample, test, "disabled channel claims no same sample");
  check(result.calibrated.failure == MqAdcCalibrationFailure::NotAttempted, test,
        "disabled channel keeps NotAttempted");
}

// A failed read stays unavailable; the ordering fix must not invent a value.
template <typename Reader>
void check_read_failure(const char* test, Reader& reader) {
  adc_stub::reset(adc_stub::kStartAttenuationAdc6db, -1);
  const MqAdcReadResult result = reader.read();

  check(!result.ok, test, "failed read is not ok");
  check(result.disconnected, test, "failed read is reported disconnected");
  check_equal_int(adc_stub::count(adc_stub::CallKind::Read), 1, test,
                  "failed read still performs exactly one analogRead");
  check_equal_int(adc_stub::count(adc_stub::CallKind::Convert), 0, test,
                  "failed read performs no conversion");
  check(!result.calibrated.available, test, "failed read carries no evidence");
  check_equal_int(static_cast<int>(result.calibrated.failure),
                  static_cast<int>(MqAdcCalibrationFailure::NotAttempted),
                  test, "failed read does not fabricate a calibration failure record");

  const adc_stub::Call* configuration =
      adc_stub::first(adc_stub::CallKind::ConfigureAttenuation);
  const adc_stub::Call* sample = adc_stub::first(adc_stub::CallKind::Read);
  check(configuration != nullptr && sample != nullptr && configuration < sample, test,
        "attenuation is configured before the attempted read");
}

// The converter must annotate a sample, never configure the channel after it.
void check_converter_is_sample_only(const char* test) {
  adc_stub::reset();
  const MqAdcCalibratedVoltage evidence =
      mq_adc_calibrated_voltage_for_pin(kMq135Pin, kSampleCounts);

  check_equal_int(adc_stub::count(adc_stub::CallKind::ConfigureAttenuation), 0, test,
                  "converter must not configure attenuation after the sample");
  check_equal_int(adc_stub::count(adc_stub::CallKind::Read), 0, test,
                  "converter must not read the ADC");
  check_equal_int(adc_stub::count(adc_stub::CallKind::ReadMilliVolts), 0, test,
                  "converter must not call analogReadMilliVolts");
  check(evidence.available, test, "converter still returns available evidence");
  check_equal_int(evidence.counts, kSampleCounts, test,
                  "converter annotates the caller's counts");
  check_equal_int(evidence.attenuation_db, 11, test,
                  "evidence reports the attenuation the adapter configured");
  check(adc_stub::last_characterized_atten == ADC_ATTEN_DB_12, test,
        "characterization uses ADC_ATTEN_DB_12");
  check(adc_stub::last_characterized_unit == ADC_UNIT_1, test,
        "characterization uses ADC1");
}

template <typename Case>
void run_case(const char* test, Case body) {
  const int before = failures;
  body(test);
  if (failures == before) std::printf("PASS %s\n", test);
}

}  // namespace

int main() {
  run_case("arduino_adapter_configures_before_single_read", [](const char* test) {
    ArduinoMqAdcReader reader(kMq135Pin);
    reader.enable_calibrated_voltage();
    check_corrected_sequence(test, reader);
  });
  run_case("target_adapter_configures_before_single_read", [](const char* test) {
    TargetMqAdcReader reader(kMq135Pin);
    reader.enable_calibrated_voltage();
    check_corrected_sequence(test, reader);
  });
  run_case("arduino_disabled_channel_keeps_legacy_behavior", [](const char* test) {
    ArduinoMqAdcReader reader(kMq135Pin);
    check_disabled_legacy(test, reader);
  });
  run_case("target_disabled_channel_keeps_legacy_behavior", [](const char* test) {
    TargetMqAdcReader reader(kMq135Pin);
    check_disabled_legacy(test, reader);
  });
  run_case("arduino_read_failure_stays_unavailable", [](const char* test) {
    ArduinoMqAdcReader reader(kMq135Pin);
    reader.enable_calibrated_voltage();
    check_read_failure(test, reader);
  });
  run_case("target_read_failure_stays_unavailable", [](const char* test) {
    TargetMqAdcReader reader(kMq135Pin);
    reader.enable_calibrated_voltage();
    check_read_failure(test, reader);
  });
  run_case("converter_does_not_configure_or_read",
           [](const char* test) { check_converter_is_sample_only(test); });

  if (failures != 0) {
    std::printf("RESULT FAILED %d assertion(s)\n", failures);
    return 1;
  }
  std::printf("RESULT PASSED 7 case(s)\n");
  return 0;
}
