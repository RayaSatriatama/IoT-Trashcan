// S2 MQG2 producer: real C++ serializer output for a normalized session,
// exercised through the same CommandRouter/session/controller path as MQG1.
// Fixtures written by this suite are SYNTHETIC/TEST_ONLY device output, never
// hand-authored JSON claiming physical origin.

#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "../test_debug_admission/admission_fixtures.h"
#include "calibration/mq135_normalized_parameters.h"
#include "calibration/calibration_replay_store.h"
#include "maintenance/ble_calibration_backend.h"
#include "network/device_control.h"
#include "network/device_control_codec.h"
#include "provisioning/ble_provisioning.h"
#include "telemetry/contract.h"
#define ARDUINO
#include "../../../src/app/device_control_executor.cpp"
#undef ARDUINO

using namespace sigap;

void setUp() {}
void tearDown() {}

namespace {

constexpr const char* kNormalizedParameters =
    "response_mode=normalized samples=3 min_duration_ms=2 max_age_ms=1000 "
    "max_variation_milli_percent=5000 circuit_mv=5000 divider_top_ohm=10000 "
    "divider_bottom_ohm=15000 topology=1 hardware_confirmed=1 "
    "reference_confirmed=1 hardware_identity=SYNTHETIC reference_environment=SYNTHETIC "
    "reference_kind=clean_air_confirmed preparation_confirmed=1 "
    "preparation_method=operator_declared heating_duration_us=180000000000 "
    "reference_temperature_centi_c=2537 reference_humidity_centi_pct=4100";

struct Clock : maintenance::ICalibrationClock,
              provisioning::BleProvisioningClock {
  uint64_t now{100};
  uint64_t now_us() const noexcept override { return now; }
};
struct QuietWifiBackend final : provisioning::IBleProvisioningBackend {
  provisioning::WifiState wifi_state() const noexcept override {
    return provisioning::WifiState::Unconfigured;
  }
  std::string_view current_ssid() const noexcept override { return {}; }
  provisioning::WifiScanStatus request_scan() noexcept override {
    return provisioning::WifiScanStatus::Failed;
  }
  provisioning::WifiScanStatus poll_scan(provisioning::WifiNetwork*,
                                          std::size_t,
                                          std::size_t& count) noexcept override {
    count = 0U;
    return provisioning::WifiScanStatus::Failed;
  }
  provisioning::WifiScanStatus list_known_wifi(provisioning::WifiNetwork*,
                                                std::size_t,
                                                std::size_t& count) noexcept override {
    count = 0U;
    return provisioning::WifiScanStatus::Ready;
  }
  bool request_connect(std::string_view, std::string_view) noexcept override {
    return false;
  }
  bool retry_wifi() noexcept override { return false; }
  void cancel() noexcept override {}
};
struct Authorizer final : calibration::ICalibrationTransportAuthorizer {
  bool allow{true};
  bool authorize(const calibration::CalibrationCommandEnvelope&) const noexcept override {
    return allow;
  }
};

bool make_https_command(std::string_view command_id, std::string_view operation,
                        std::string_view payload, uint64_t expires_at_us,
                        network::DeviceControlCommand& command) {
  if (payload.size() > command.payload.size()) return false;
  command = network::DeviceControlCommand{};
  if (!command.command_id.assign(command_id) ||
      !command.device_id.assign("0000000000000007") ||
      !command.operator_id.assign("operator-1") ||
      !command.session_id.assign("normalized-s2-session") ||
      !command.sensor_id.assign("mq135_gas") ||
      !command.operation.assign(operation)) {
    return false;
  }
  std::copy(payload.begin(), payload.end(), command.payload.begin());
  command.payload_size = payload.size();
  command.expires_at_us = expires_at_us;
  std::string digest{"sha256:"};
  constexpr char kHex[] = "0123456789abcdef";
  for (const uint8_t byte : telemetry_contract::sha256(payload)) {
    digest += kHex[byte >> 4U];
    digest += kHex[byte & 0x0FU];
  }
  return command.request_hash.assign(digest);
}

struct Evidence : calibration::ICalibrationEvidenceSource {
  calibration::MqTrial value{};
  bool latest(calibration::CalibrationTarget target, uint64_t after,
              calibration::TrialObservation& out,
              uint64_t& sequence) const noexcept override {
    if (target != calibration::CalibrationTarget::Mq135Gas ||
        value.source_sequence <= after) {
      return false;
    }
    out = value;
    sequence = value.source_sequence;
    return true;
  }
  bool diagnostic(calibration::CalibrationTarget,
                  calibration::DiagnosticEvidence& out) const noexcept override {
    out.available = true;
    out.sensor = acquisition::SensorId::Mq135;
    out.quality = value.quality;
    return true;
  }
};

struct Fixture {
  admission_test::ConfigStore cs{};
  config::ConfigManager config{cs, {8ULL * 1024 * 1024, 0}};
  admission_test::Secrets ss{};
  provisioning::PersistentSecretStore secrets{ss};
  acquisition::MemoryProfileStore store{};
  acquisition::ProfilePersistence persistence{store};
  acquisition::ProfileManager profiles{persistence};
  acquisition::ProfileCodec codec{};
  acquisition::BootId boot{};
  Evidence evidence{};
  Clock clock{};
  Authorizer auth{};
  calibration::MemoryCalibrationReplayStore replay_store{};
  maintenance::CommandRouter router{config, secrets, &profiles, &evidence, nullptr,
                                    nullptr, &boot};
  calibration::CalibrationTransportController controller{router, 7, &auth,
                                                        &replay_store};
  maintenance::CommandRouterBleCalibrationBackend ble{router, clock, &controller};
  ApplicationCalibrationExecutor https{router, controller};

  Fixture() {
    (void)config.begin();
    boot.present = true;
    boot.bytes.fill(0x11);
    acquisition::MqAdcProfile a{};
    a.manifest.kind = acquisition::ProfileKind::Adc;
    a.manifest.target_id = 8;
    a.manifest.profile_version = 1;
    a.manifest.validity = acquisition::ProfileValidity::TestOnly;
    a.manifest.provenance.present = true;
    a.manifest.provenance.evidence_count = 3;
    a.reference_mv = 3000;
    a.full_scale_counts = 3000;
    a.divider_numerator = 3;
    a.divider_denominator = 5;
    acquisition::EncodedProfile encoded{};
    std::printf("DBG enc=%d\n", (int)codec.encode(a, encoded));
    std::printf("DBG stage=%d\n", (int)profiles.stage_import(acquisition::ProfileTarget::Mq135, encoded));
    std::printf("DBG preview=%d\n", (int)profiles.preview(acquisition::ProfileTarget::Mq135));
    std::printf("DBG commit=%d\n", (int)profiles.commit(acquisition::ProfileTarget::Mq135));
    std::printf("DBG load=%d\n", (int)profiles.load(acquisition::kMq135GasKey));
    std::printf("DBG decode=%d\n", (int)codec.decode(encoded, a));
    evidence.value.source_profile = a.ref();
    evidence.value.source_boot_id = boot;
    evidence.value.raw_present = evidence.value.identity_valid = true;
    evidence.value.quality = acquisition::SampleQuality::Ready;
    set_sample(1500U, 1500.0F);
    router.set_calibration_transport_controller(&controller);
  }

  // Same-sample calibrated evidence with an explicit eFuse TP-FIT conversion.
  void set_sample(uint16_t counts, float pin_mv) {
    auto& v = evidence.value;
    v.adc_counts = counts;
    auto& cal = v.calibrated;
    cal = acquisition::MqAdcCalibratedVoltage{};
    cal.available = true;
    cal.same_sample = true;
    cal.method = acquisition::MqAdcCalibrationMethod::EspAdcCalEfuseTpFit;
    cal.failure = acquisition::MqAdcCalibrationFailure::None;
    cal.unit = acquisition::MqAdcVoltageUnit::Millivolt;
    cal.resolution_bits = 12;
    cal.attenuation_db = 11;
    cal.adc_unit = 1;
    cal.adc_channel = 4;
    cal.counts = counts;
    cal.pin_mv = pin_mv;
  }
  void next() {
    clock.now += 1000;
    ++evidence.value.source_sequence;
    evidence.value.observed_at_us = clock.now;
  }
  bool line(const std::string& text, std::string* output = nullptr) {
    maintenance::MaintenanceParser parser{};
    maintenance::ParseEvent event{};
    for (char byte : text) (void)parser.consume(byte, event);
    if (!parser.consume('\n', event) ||
        event.status != maintenance::CommandStatus::Accepted) {
      return false;
    }
    maintenance::MaintenanceResponse response{};
    const auto result = router.execute(event.command, clock.now, response);
    if (output) output->assign(response.view());
    return result == maintenance::CommandStatus::Accepted;
  }
  bool radio(const std::string& text, std::string* output = nullptr) {
    provisioning::BleCommand parsed{};
    const auto ps = provisioning::BleCommandParser::parse(text, parsed);
    std::printf("DBG radio parse=%d text=%s\n", (int)ps, text.c_str());
    if (ps != provisioning::BleParseStatus::Accepted) return false;
    char buffer[2048]{};
    std::size_t size{};
    const bool ok = ble.execute(text, buffer, sizeof(buffer), size);
    std::printf("DBG radio ok=%d size=%u first=%s\n", (int)ok, (unsigned)size, size ? buffer : "");
    if (output) output->assign(buffer, size);
    return ok;
  }
  bool apply_criteria(const std::string& parameters = kNormalizedParameters) {
    std::string values = parameters;
    while (!values.empty()) {
      auto end = values.find(' ');
      auto token = values.substr(0, end);
      auto at = token.find('=');
      if (!line("calibration criteria " + token.substr(0, at) + " " +
                token.substr(at + 1))) {
        return false;
      }
      if (end == std::string::npos) break;
      values.erase(0, end + 1);
    }
    return true;
  }
  bool capture_reference(unsigned count = 3U) {
    for (unsigned i = 0; i < count; ++i) {
      next();
      if (!line("calibration capture reference")) return false;
    }
    return true;
  }
  bool commit_normalized(const std::string& parameters = kNormalizedParameters) {
    if (!line("calibration start mq135_gas")) { std::printf("DBG start failed\n"); return false; }
    if (!apply_criteria(parameters)) { std::printf("DBG criteria failed\n"); return false; }
    if (!capture_reference()) { std::printf("DBG capture failed\n"); return false; }
    if (!line("calibration validate")) { std::printf("DBG validate failed\n"); return false; }
    if (!line("calibration preview")) { std::printf("DBG preview failed\n"); return false; }
    if (!line("calibration commit")) { std::printf("DBG commit failed\n"); return false; }
    return true;
  }
  bool set_adc_profile_version(uint32_t version) {
    acquisition::ActiveProfileSnapshot active{};
    if (!profiles.snapshot(acquisition::ProfileTarget::Mq135, active)) return false;
    acquisition::MqAdcProfile profile{};
    if (!codec.decode(active.encoded, profile)) return false;
    profile.manifest.profile_version = version;
    acquisition::EncodedProfile encoded{};
    if (!codec.encode(profile, encoded) ||
        !profiles.stage_import(acquisition::ProfileTarget::Mq135, encoded) ||
        !profiles.validate(acquisition::ProfileTarget::Mq135) ||
        !profiles.preview(acquisition::ProfileTarget::Mq135) ||
        !profiles.commit(acquisition::ProfileTarget::Mq135)) {
      return false;
    }
    if (!profiles.snapshot(acquisition::ProfileTarget::Mq135, active)) return false;
    evidence.value.source_profile = active.ref;
    return true;
  }
  bool set_gas_profile_version(uint32_t version) {
    acquisition::ActiveProfileSnapshot active{};
    if (!profiles.snapshot(acquisition::kMq135GasKey, active)) return false;
    acquisition::Mq135NormalizedProfile profile{};
    if (!acquisition::decode_mq135_normalized_profile(active.encoded, profile)) {
      return false;
    }
    profile.manifest.profile_version = version;
    acquisition::EncodedProfile encoded{};
    return acquisition::encode_mq135_normalized_profile(profile, encoded) &&
           profiles.stage_import(acquisition::kMq135GasKey, encoded) &&
           profiles.validate(acquisition::kMq135GasKey) &&
           profiles.preview(acquisition::kMq135GasKey) &&
           profiles.commit(acquisition::kMq135GasKey);
  }
  std::string evidence_json() {
    std::string output;
    // Gas control evidence is read through the shared BLE/HTTPS controller
    // (the same CAL1 envelope path MQG1 uses), not a USB maintenance line.
    (void)radio("CAL EVIDENCE mq135_gas", &output);
    return output;
  }
  // The same controller path, but returning only the device-authored MQG2
  // document: the controller result is serialized by the real CommandRouter.
  // This is the exact byte sequence a consumer receives as the `data` member.
  std::string producer_json(uint64_t request_id = 0U) {
    calibration::CalibrationCommandEnvelope request{};
    request.device_id = 7U;
    request.principal_id = 1U;
    request.transport = calibration::CalibrationTransport::Ble;
    request.target = calibration::CalibrationTarget::Mq135Gas;
    request.request_id = request_id != 0U
                             ? request_id
                             : 900U + evidence.value.source_sequence;
    request.expires_at_us = clock.now + 1000000;
    request.operation = calibration::CalibrationOperation::GetEvidence;
    calibration::CalibrationResultEnvelope result{};
    if (!controller.execute(request, clock.now, result)) return {};
    maintenance::MaintenanceResponse payload{};
    const bool emitted = result.normalized_gas.present
                             ? router.append_mq135_normalized_result(
                                   payload, result.normalized_gas)
                             : router.append_mq135_gas_result(payload, result.gas);
    if (!emitted) return {};
    return std::string(payload.view());
  }
  // Maximum supported valid MQG2, produced by committing a normalized session
  // and then routing the current diagnostic through the shared controller.
  std::string valid_worst_case_json() {
    if (!set_adc_profile_version(UINT32_MAX)) return {};
    std::string parameters{kNormalizedParameters};
    const std::string old_hardware{"hardware_identity=SYNTHETIC"};
    const std::string new_hardware{"hardware_identity=SYNTHETIC-" +
                                   std::string(21U, 'H')};
    const std::string old_environment{"reference_environment=SYNTHETIC"};
    const std::string new_environment{"reference_environment=REFERENCE-" +
                                      std::string(21U, 'R')};
    auto at = parameters.find(old_hardware);
    if (at == std::string::npos) return {};
    parameters.replace(at, old_hardware.size(), new_hardware);
    at = parameters.find(old_environment);
    if (at == std::string::npos) return {};
    parameters.replace(at, old_environment.size(), new_environment);
    at = parameters.find("max_age_ms=1000");
    if (at == std::string::npos) return {};
    parameters.replace(at, std::strlen("max_age_ms=1000"),
                       "max_age_ms=9223372036854775");
    at = parameters.find("heating_duration_us=180000000000");
    if (at == std::string::npos) return {};
    parameters.replace(at, std::strlen("heating_duration_us=180000000000"),
                       "heating_duration_us=9223372036854775807");
    if (!commit_normalized(parameters) || !set_gas_profile_version(UINT32_MAX)) {
      return {};
    }
    next();
    set_sample(2308U, 2308.0F);
    clock.now = UINT64_MAX - 1000000ULL;
    evidence.value.observed_at_us = clock.now - 1000ULL;
    evidence.value.source_sequence = UINT64_MAX;
    return producer_json();
  }
  // Retained intentionally invalid stress input; never used as a positive
  // fixture or as proof that a valid response fits transport bounds.
  std::string serializer_stress_json() {
    acquisition::Mq135NormalizedDiagnostic d{};
    d.present = true;
    const char* q = "OUT_OF_RANGE";
    std::copy_n(q, std::strlen(q), d.quality.begin());
    const char* r = "ADC_PROFILE_CHANGED";
    std::copy_n(r, std::strlen(r), d.reason.begin());
    d.raw_present = true; d.raw_counts = 4095; d.adc_available = true;
    d.pin_present = d.ao_present = d.same_sample = true;
    d.pin_mv = 3299.999F; d.ao_mv = 5499.998F;
    const char* f = "CHARACTERIZE_UNSUPPORTED";  // longest producer failure code
    std::copy_n(f, std::min(std::strlen(f), d.adc_failure.size() - 1U),
                d.adc_failure.begin());
    d.resolution_bits = 12; d.attenuation_db = 11; d.adc_unit = 1; d.adc_channel = 4;
    d.circuit_present = true; d.circuit_mv = 4294967295U;
    d.divider_top_ohm = 4294967295U; d.divider_bottom_ohm = 4294967295U;
    d.topology = acquisition::Mq135Topology::Buffered; d.hardware_confirmed = true;
    const char* id = "SYNTHETIC-HW-IDENTITY-31CHARS-X";
    std::copy_n(id, std::strlen(id), d.hardware_identity.begin());
    d.q_present = d.q0_present = d.ratio_present = true;
    d.q = 1.2345678901234; d.q0 = 1.2345678901234F; d.ratio = 0.999999999999;
    d.reference_kind = acquisition::Mq135ReferenceKind::CleanAirConfirmed;
    d.baseline_valid = true; d.ppm_present = true; d.ppm = 999.999999999;
    // Longest documented status label (27 chars), not a shorter alias.
    const char* st = "ADC_CALIBRATION_UNAVAILABLE";
    std::copy_n(st, std::min(std::strlen(st), d.nh3_status.size() - 1U),
                d.nh3_status.begin());
    d.curve_present = true; d.curve_id = 0x4E48U; d.curve_version = 2U;
    d.curve_source_sha256.fill(0xABU);
    const char* ps = "TEST_ONLY";
    std::copy_n(ps, std::strlen(ps), d.profile_state.begin());
    d.profile.present = true; d.profile.profile_version = 4294967295U;
    d.profile.hash.fill(0xCDU);
    d.previous_profile.present = true;
    d.previous_profile.profile_version = 4294967295U;
    d.previous_profile.hash.fill(0xEFU);
    d.adc_profile.present = true; d.adc_profile.profile_version = 4294967295U;
    d.adc_profile.hash.fill(0x12U);
    d.source_present = true; d.source_boot_id.present = true;
    d.source_boot_id.bytes.fill(0xFFU);
    d.source_sequence = UINT64_MAX; d.source_us = UINT64_MAX;
    d.response_us = UINT64_MAX; d.age_known = true; d.fresh = true;
    d.max_age_us = UINT64_MAX;
    const char* sq = "WARMING_UP";
    std::copy_n(sq, std::strlen(sq), d.source_quality.begin());
    const char* env = "SYNTHETIC-REF-ENVIRONMENT-31CH";
    std::copy_n(env, std::strlen(env), d.reference_environment.begin());
    d.temperature_present = d.humidity_present = d.conditions_declared = true;
    d.temperature_centi_c = -32768; d.humidity_centi_pct = 10000U;
    d.heating_duration_us = UINT64_MAX; d.preparation_confirmed = true;
    d.preparation_method = acquisition::Mq135PreparationMethod::OperatorDeclared;
    d.sample_count = 65535U; d.variation_percent = 100.0F;
    d.calibration_recorded_at.available = true;
    d.calibration_recorded_at.epoch_ms = INT64_MIN;
    d.calibration_time_quality = acquisition::TimeQuality::Synchronized;
    maintenance::MaintenanceResponse out{};
    const bool ok = router.append_mq135_normalized_result(out, d);
    std::printf("DBG worst ok=%d size=%u tail=%s\n", (int)ok, (unsigned)out.size(), out.c_str() + (out.size() > 120 ? out.size() - 120 : 0));
    if (!ok) return {};
    return std::string(out.view());
  }
  // Direct serializer measurement used to size transport bounds.
  std::string raw_result_json() {
    acquisition::Mq135NormalizedDiagnostic d{};
    if (!router.normalized_gas_diagnostic(calibration::CalibrationTarget::Mq135Gas,
                                          clock.now, d)) return {};
    maintenance::MaintenanceResponse out{};
    const bool ok = router.append_mq135_normalized_result(out, d);
    std::printf("DBG worst ok=%d size=%u tail=%s\n", (int)ok, (unsigned)out.size(), out.c_str() + (out.size() > 120 ? out.size() - 120 : 0));
    if (!ok) return {};
    return std::string(out.view());
  }
};

std::string hex(const config::ConfigHash& hash) {
  const char digits[] = "0123456789abcdef";
  std::string text;
  for (auto b : hash) {
    text += digits[b >> 4];
    text += digits[b & 15];
  }
  return text;
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Returns the JSON number that follows `key`, or NaN when the value is null,
// absent or not numeric. Used to assert the actual computed quantity instead of
// a string prefix.
double number_after(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto at = json.find(needle);
  if (at == std::string::npos) return std::nan("");
  const char* begin = json.c_str() + at + needle.size();
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  if (end == begin) return std::nan("");
  return value;
}

// Unity's double support is not compiled into this runner, so compare with an
// explicit absolute tolerance instead of TEST_ASSERT_DOUBLE_WITHIN.
bool near(double actual, double expected, double tolerance) {
  return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
}

// Fixture freezing. When SIGAP_MQG2_FIXTURE_DIR is set the exact bytes this
// suite produced are written there (the only supported way to (re)generate the
// released fixtures). Otherwise, if the committed fixture exists, the produced
// bytes must match it byte-for-byte, so a hand-authored or stale fixture can
// never pass as device output.
std::string fixture_dir() {
  const char* env = std::getenv("SIGAP_MQG2_FIXTURE_DIR");
  return env != nullptr ? std::string(env) : std::string{};
}

std::string fixture_path(const char* name) {
  const std::string dir = fixture_dir();
  if (!dir.empty()) return dir + "/" + name + ".json";
  const char* roots[] = {"../contracts/calibration/mq135-nh3/v2/fixtures",
                         "contracts/calibration/mq135-nh3/v2/fixtures",
                         "../../contracts/calibration/mq135-nh3/v2/fixtures"};
  for (const char* root : roots) {
    const std::string candidate = std::string(root) + "/" + name + ".json";
    if (std::FILE* probe = std::fopen(candidate.c_str(), "rb")) {
      std::fclose(probe);
      return candidate;
    }
  }
  return {};
}

bool read_file(const std::string& path, std::string& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  bytes.clear();
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    bytes.append(buffer, read);
  }
  std::fclose(file);
  return true;
}

bool write_file(const std::string& path, const std::string& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return written == bytes.size();
}

// Records the exact producer bytes for `name` and returns their length so the
// caller can assert the measured transport bounds.
std::size_t freeze(const char* name, const std::string& json) {
  TEST_ASSERT_FALSE_MESSAGE(json.empty(), name);
  if (!fixture_dir().empty()) {
    TEST_ASSERT_TRUE_MESSAGE(write_file(fixture_path(name), json), name);
  } else {
    const std::string path = fixture_path(name);
    TEST_ASSERT_TRUE_MESSAGE(!path.empty(), name);
    std::string committed;
    TEST_ASSERT_TRUE_MESSAGE(read_file(path, committed), path.c_str());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(committed.size(), json.size(), path.c_str());
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(committed.data(), json.data(), json.size(),
                                     path.c_str());
  }
  std::printf("MQG2_FIXTURE_BYTES name=%s bytes=%u\n", name, (unsigned)json.size());
  return json.size();
}

}  // namespace

// A normalized session emits MQG2 with every documented group present and a
// dimensionless q0/ratio from the calibrated same-sample voltage.
//
// The reference window is captured at pin 1500 mV (AO 2500 mV, q0 = 1.0). The
// live sample is then read at pin 2308 mV (AO 3846.667 mV, q = 0.29982669), so
// the released ratio q/q0 = 0.29982669 lands inside the documented Winsen v1.6
// Figure 3 domain 0.0893..0.6559 and the estimator must return a finite ppm.
void test_normalized_session_emits_complete_mqg2_groups() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.next();
  f.set_sample(2308U, 2308.0F);
  const std::string json = f.raw_result_json();
  std::printf("DBG JSONLEN=%u\nDBG JSON=%s\n", (unsigned)json.size(), json.c_str());
  TEST_ASSERT_TRUE(contains(json, "\"schema\":\"MQG2\""));
  TEST_ASSERT_TRUE(contains(json, "\"sensor\":\"mq135\""));
  TEST_ASSERT_TRUE(contains(json, "\"adc\":{"));
  TEST_ASSERT_TRUE(contains(json, "\"electrical\":{"));
  TEST_ASSERT_TRUE(contains(json, "\"response\":{"));
  TEST_ASSERT_TRUE(contains(json, "\"nh3\":{"));
  TEST_ASSERT_TRUE(contains(json, "\"profiles\":{"));
  TEST_ASSERT_TRUE(contains(json, "\"source\":{"));
  TEST_ASSERT_TRUE(contains(json, "\"reference\":{"));
  TEST_ASSERT_TRUE(contains(json, "\"method\":\"EFUSE_TP_FIT\""));
  TEST_ASSERT_TRUE(contains(json, "\"available\":true"));
  TEST_ASSERT_TRUE(contains(json, "\"same_sample\":true"));
  // Assert parsed numeric values; their round-trip decimal representation is
  // deliberately not tied to a particular number of printed digits.
  TEST_ASSERT_TRUE(near(number_after(json, "q"), 0.29982669, 1e-8));
  TEST_ASSERT_TRUE(near(number_after(json, "q0"), 1.0, 1e-6));
  TEST_ASSERT_TRUE(near(number_after(json, "ratio"), 0.29982669, 1e-8));
  TEST_ASSERT_TRUE(contains(json, "\"reference_kind\":\"CLEAN_AIR_CONFIRMED\""));
  TEST_ASSERT_TRUE(contains(json, "\"production_eligible\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"method\":\"PIECEWISE_LOG_LOG\""));
  TEST_ASSERT_TRUE(contains(json, "\"status\":\"IN_RANGE\""));
  // IN_RANGE is only legitimate with a finite ppm inside the reviewed domain.
  const double ppm = number_after(json, "ppm");
  TEST_ASSERT_TRUE(std::isfinite(ppm));
  // ratio 0.29982669 is bracketed by 0.3114 (70 ppm) and 0.2668 (100 ppm).
  TEST_ASSERT_TRUE(ppm > 70.0 && ppm < 100.0);
  TEST_ASSERT_FALSE(contains(json, "\"ppm\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"conditions_source\":\"DECLARED\""));
  TEST_ASSERT_TRUE(contains(json, "\"preparation_method\":\"OPERATOR_DECLARED\""));
  TEST_ASSERT_TRUE(contains(json, "\"state\":\"TEST_ONLY\""));
  // No legacy ohms aliases may leak into MQG2.
  TEST_ASSERT_FALSE(contains(json, "rs_ohm"));
  TEST_ASSERT_FALSE(contains(json, "r0_ohm"));
  TEST_ASSERT_FALSE(contains(json, "module_load_ohm"));
  std::printf("MQG2_FIXTURE_IN_RANGE=%s\n", json.c_str());
  freeze("in-range", json);
}

// An OPERATIONAL baseline keeps q/q0/ratio but never publishes a ppm.
void test_operational_baseline_keeps_ratio_and_nulls_ppm() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.line("maintenance enter"));
  TEST_ASSERT_TRUE(f.line("calibration start mq135_gas"));
  std::string operational{kNormalizedParameters};
  const std::string from = "reference_kind=clean_air_confirmed preparation_confirmed=1 "
                           "preparation_method=operator_declared heating_duration_us=180000000000";
  const std::string to = "reference_kind=operational preparation_confirmed=0 "
                         "preparation_method=unknown heating_duration_us=0";
  operational.replace(operational.find(from), from.size(), to);
  TEST_ASSERT_TRUE(f.apply_criteria(operational));
  TEST_ASSERT_TRUE(f.capture_reference());
  TEST_ASSERT_TRUE(f.line("calibration validate"));
  TEST_ASSERT_TRUE(f.line("calibration preview"));
  TEST_ASSERT_TRUE(f.line("calibration commit"));
  f.next();
  f.set_sample(2308U, 2308.0F);
  // The BLE path must carry the identical document in its `data` member.
  const std::string envelope = f.evidence_json();
  TEST_ASSERT_TRUE(contains(envelope, "CAL1|op=EVIDENCE|status=ACCEPTED"));
  const std::string json = f.producer_json();
  TEST_ASSERT_TRUE(contains(envelope, json));
  TEST_ASSERT_TRUE(contains(json, "\"reference_kind\":\"OPERATIONAL\""));
  TEST_ASSERT_TRUE(contains(json, "\"status\":\"REFERENCE_NOT_CLEAN_AIR\""));
  TEST_ASSERT_TRUE(contains(json, "\"ppm\":null"));
  // The ratio is a legitimate in-domain value; only the reference kind blocks
  // the ppm, so this exercises the intended guard rather than a range failure.
  TEST_ASSERT_TRUE(near(number_after(json, "ratio"), 0.29982669, 1e-8));
  TEST_ASSERT_TRUE(near(number_after(json, "q0"), 1.0, 1e-6));
  std::printf("MQG2_FIXTURE_OPERATIONAL=%s\n", json.c_str());
  freeze("operational-baseline", json);
}

// Both out-of-range directions keep the legitimate ratio and null the ppm, and
// a voltage at or above Vc is a real invalid-ratio case.
void test_both_out_of_range_directions_keep_ratio_and_null_ppm() {
  Fixture f{};
  // Reference window at pin 1500 mV => q0 = 1.0, so ratio == q.
  TEST_ASSERT_TRUE(f.commit_normalized());
  // ratio 9.0 > 0.6559 => below the 10 ppm end of the domain.
  f.next();
  f.set_sample(300U, 300.0F);  // AO 500 mV -> q = 9.0 => ratio 9
  const std::string below = f.producer_json();
  TEST_ASSERT_TRUE(contains(below, "\"status\":\"BELOW_RANGE\""));
  TEST_ASSERT_TRUE(contains(below, "\"ppm\":null"));
  TEST_ASSERT_TRUE(near(number_after(below, "ratio"), 9.0, 1e-9));
  TEST_ASSERT_TRUE(near(number_after(below, "q"), 9.0, 1e-9));
  std::printf("MQG2_FIXTURE_BELOW_RANGE=%s\n", below.c_str());
  freeze("below-range", below);

  // ratio 0.07142857 < 0.0893 => above the 1000 ppm end of the domain.
  f.next();
  f.set_sample(2800U, 2800.0F);  // AO 4666.667 mV -> q = 0.07142857
  const std::string above = f.producer_json();
  TEST_ASSERT_TRUE(contains(above, "\"status\":\"ABOVE_RANGE\""));
  TEST_ASSERT_TRUE(contains(above, "\"ppm\":null"));
  TEST_ASSERT_TRUE(near(number_after(above, "ratio"), 0.0714285714, 1e-8));
  std::printf("MQG2_FIXTURE_ABOVE_RANGE=%s\n", above.c_str());
  freeze("above-range", above);

  // pin 3100 mV -> AO 5166.667 mV >= Vc 5000 mV, so q cannot be formed at all.
  // This is the real AO_OUT_OF_RANGE guard, not a domain-edge value.
  f.next();
  f.set_sample(3100U, 3100.0F);
  const std::string invalid = f.producer_json();
  TEST_ASSERT_TRUE(contains(invalid, "\"status\":\"INVALID_RATIO\""));
  TEST_ASSERT_TRUE(contains(invalid, "\"q\":null"));
  TEST_ASSERT_TRUE(contains(invalid, "\"ratio\":null"));
  TEST_ASSERT_TRUE(contains(invalid, "\"ao_mv\":null"));
  TEST_ASSERT_TRUE(contains(invalid, "\"ppm\":null"));
  std::printf("MQG2_FIXTURE_INVALID_RATIO=%s\n", invalid.c_str());
  freeze("invalid-ratio", invalid);
}

// A same-sample calibration record cannot admit a normalized measurement when
// the source says its raw sample is absent.
void test_raw_absent_sample_is_not_admitted() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.next();
  f.evidence.value.raw_present = false;
  const std::string json = f.producer_json();
  TEST_ASSERT_TRUE(contains(json, "\"raw_counts\":null"));
  // ADC availability is checked before source readiness: without raw counts,
  // same-sample calibration cannot be admitted even if stale metadata says OK.
  TEST_ASSERT_TRUE(contains(json, "\"available\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"method\":\"UNAVAILABLE\""));
  TEST_ASSERT_TRUE(contains(json, "\"failure\":\"NOT_ATTEMPTED\""));
  TEST_ASSERT_TRUE(contains(json, "\"status\":\"ADC_CALIBRATION_UNAVAILABLE\""));
  TEST_ASSERT_TRUE(contains(json, "\"q\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"ratio\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"ppm\":null"));
  freeze("negative/raw-absent", json);
}

void test_normalized_source_identity_sequence_clock_and_profile_guards() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.next();
  f.set_sample(2308U, 2308.0F);
  const auto admitted = f.evidence.value;
  const auto rejected = [&f](uint64_t request_id, const char* status) {
    const std::string json = f.producer_json(request_id);
    return contains(json, std::string("\"status\":\"") + status + "\"") &&
           contains(json, "\"q\":null") &&
           contains(json, "\"ratio\":null") &&
           contains(json, "\"ppm\":null");
  };

  f.evidence.value.identity_valid = false;
  TEST_ASSERT_TRUE(rejected(5101U, "SOURCE_NOT_READY"));
  f.evidence.value = admitted;

  f.evidence.value.source_boot_id.bytes[0] ^= 1U;
  TEST_ASSERT_TRUE(rejected(5102U, "SOURCE_NOT_READY"));
  f.evidence.value = admitted;

  f.evidence.value.observed_at_us = f.clock.now + 1U;
  TEST_ASSERT_TRUE(rejected(5103U, "SOURCE_NOT_READY"));
  f.evidence.value = admitted;

  f.evidence.value.source_sequence = 0U;
  TEST_ASSERT_TRUE(rejected(5104U, "ADC_CALIBRATION_UNAVAILABLE"));
  const std::string no_sequence = f.producer_json(5104U);
  TEST_ASSERT_TRUE(contains(no_sequence, "\"sequence\":null"));
  freeze("negative/sequence-zero", no_sequence);
  f.evidence.value = admitted;

  f.evidence.value.source_profile.hash[0] ^= 1U;
  TEST_ASSERT_TRUE(rejected(5105U, "PROFILE_INVALID"));
  const std::string wrong_profile = f.producer_json(5105U);
  freeze("negative/source-profile-mismatch", wrong_profile);
}

void test_normalized_adc_counts_method_settings_and_saturation_guards() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.next();
  f.set_sample(2308U, 2308.0F);
  const auto admitted = f.evidence.value;
  for (uint32_t variant = 0U; variant < 11U; ++variant) {
    f.evidence.value = admitted;
    auto& source = f.evidence.value;
    auto& cal = source.calibrated;
    switch (variant) {
      case 0U: cal.same_sample = false; break;
      case 1U: cal.method = acquisition::MqAdcCalibrationMethod::None; break;
      case 2U: cal.failure = acquisition::MqAdcCalibrationFailure::ConversionFailed; break;
      case 3U: cal.unit = acquisition::MqAdcVoltageUnit::None; break;
      case 4U: cal.resolution_bits = 11U; break;
      case 5U: cal.attenuation_db = 10U; break;
      case 6U: cal.adc_unit = 0U; break;
      case 7U: cal.adc_channel = 3U; break;
      case 8U: ++cal.counts; break;
      case 9U: cal.pin_mv = 3301.0F; break;
      case 10U: f.set_sample(acquisition::kMqAdcMaxCounts12Bit, 3300.0F); break;
      default: break;
    }
    const std::string json = f.producer_json(5200U + variant);
    TEST_ASSERT_TRUE(contains(json, "\"status\":\"ADC_CALIBRATION_UNAVAILABLE\""));
    TEST_ASSERT_TRUE(contains(json, "\"q\":null"));
    TEST_ASSERT_TRUE(contains(json, "\"ratio\":null"));
    TEST_ASSERT_TRUE(contains(json, "\"ppm\":null"));
    if (variant == 10U) {
      TEST_ASSERT_TRUE(contains(json, "\"raw_counts\":4095"));
      TEST_ASSERT_TRUE(contains(json, "\"available\":false"));
      TEST_ASSERT_TRUE(contains(json, "\"method\":\"UNAVAILABLE\""));
      TEST_ASSERT_TRUE(contains(json, "\"failure\":\"RAW_OUT_OF_RANGE\""));
      TEST_ASSERT_TRUE(contains(json, "\"pin_mv\":null"));
      freeze("negative/adc-saturated", json);
    }
  }
}

// Unavailable ADC calibration is explicit, never a 3300 mV fallback.
void test_unavailable_calibration_is_explicit_not_a_fallback() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.next();
  auto& cal = f.evidence.value.calibrated;
  cal.available = false;
  cal.same_sample = false;
  cal.method = acquisition::MqAdcCalibrationMethod::None;
  cal.failure = acquisition::MqAdcCalibrationFailure::EfuseUnsupported;
  cal.pin_mv = 0.0F;
  // The mutation must reach the served sample: same sequence, same counts.
  const std::string json = f.producer_json();
  TEST_ASSERT_TRUE(contains(json, "\"raw_counts\":1500"));
  TEST_ASSERT_TRUE(contains(json, "\"sequence\":\"4\""));
  TEST_ASSERT_TRUE(contains(json, "\"available\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"method\":\"UNAVAILABLE\""));
  TEST_ASSERT_TRUE(contains(json, "\"failure\":\"EFUSE_UNSUPPORTED\""));
  TEST_ASSERT_TRUE(contains(json, "\"pin_mv\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"ao_mv\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"status\":\"ADC_CALIBRATION_UNAVAILABLE\""));
  TEST_ASSERT_TRUE(contains(json, "\"q\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"ratio\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"ppm\":null"));
  std::printf("MQG2_FIXTURE_ADC_UNAVAILABLE=%s\n", json.c_str());
  freeze("adc-unavailable", json);
}

void test_source_age_boundary_is_inclusive_and_overrun_is_stale() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.next();
  f.set_sample(2308U, 2308.0F);
  const uint64_t observed_at = f.evidence.value.observed_at_us;
  const uint64_t max_age = 1000000ULL;

  f.clock.now = observed_at + max_age;
  const std::string at_boundary = f.producer_json(5301U);
  TEST_ASSERT_TRUE(contains(at_boundary, "\"fresh\":true"));
  TEST_ASSERT_TRUE(contains(at_boundary, "\"age_us\":\"1000000\""));
  TEST_ASSERT_TRUE(contains(at_boundary, "\"status\":\"IN_RANGE\""));
  freeze("age-at-inclusive-boundary", at_boundary);

  ++f.clock.now;
  const std::string over_boundary = f.producer_json(5302U);
  TEST_ASSERT_TRUE(contains(over_boundary, "\"fresh\":false"));
  TEST_ASSERT_TRUE(contains(over_boundary, "\"status\":\"SOURCE_STALE\""));
  TEST_ASSERT_TRUE(contains(over_boundary, "\"q\":null"));
  TEST_ASSERT_TRUE(contains(over_boundary, "\"ratio\":null"));
  TEST_ASSERT_TRUE(contains(over_boundary, "\"ppm\":null"));
  freeze("negative/age-over-boundary", over_boundary);
}

// A stale source nulls the current quantities but keeps the stored q0 baseline.
void test_stale_source_nulls_current_quantities_but_keeps_q0() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.clock.now += 5000000;  // far beyond max_age_ms=1000
  const std::string json = f.producer_json();
  TEST_ASSERT_TRUE(contains(json, "\"status\":\"SOURCE_STALE\""));
  TEST_ASSERT_TRUE(contains(json, "\"q\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"ratio\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"ppm\":null"));
  TEST_ASSERT_TRUE(contains(json, "\"q0\":1"));
  TEST_ASSERT_TRUE(contains(json, "\"baseline_valid\":true"));
  std::printf("MQG2_FIXTURE_STALE=%s\n", json.c_str());
  freeze("stale-source", json);
}

// The legacy MQG1 contract stays readable when no normalized mode was selected.
void test_legacy_mqg1_path_remains_when_no_normalized_mode_selected() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.line("maintenance enter"));
  const std::string envelope = f.evidence_json();
  TEST_ASSERT_TRUE(contains(envelope, "\"schema\":\"MQG1\""));
  TEST_ASSERT_FALSE(contains(envelope, "\"schema\":\"MQG2\""));
  const std::string json = f.producer_json();
  TEST_ASSERT_TRUE(contains(json, "\"schema\":\"MQG1\""));
  TEST_ASSERT_FALSE(contains(json, "\"schema\":\"MQG2\""));
  freeze("legacy-mqg1", json);
}

void test_normalized_evidence_replay_keeps_original_snapshot() {
  Fixture f{};
  TEST_ASSERT_TRUE(f.commit_normalized());
  f.next();
  f.set_sample(2308U, 2308.0F);
  const std::string original = f.producer_json(123456U);
  TEST_ASSERT_TRUE(contains(original, "\"sequence\":\"4\""));
  TEST_ASSERT_TRUE(contains(original, "\"response_monotonic_us\":\"4100\""));

  f.next();
  f.set_sample(2800U, 2800.0F);
  const std::string replayed = f.producer_json(123456U);
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(original.size()),
                         static_cast<unsigned>(replayed.size()));
  TEST_ASSERT_EQUAL_MEMORY(original.data(), replayed.data(), original.size());
  TEST_ASSERT_TRUE(contains(replayed, "\"sequence\":\"4\""));
  TEST_ASSERT_TRUE(contains(replayed, "\"response_monotonic_us\":\"4100\""));

  const std::string current = f.producer_json(123457U);
  TEST_ASSERT_TRUE(contains(current, "\"sequence\":\"5\""));
  TEST_ASSERT_TRUE(contains(current, "\"status\":\"ABOVE_RANGE\""));
}

void test_normalized_controller_shares_owner_and_expires() {
  Fixture f{};
  calibration::CalibrationCommandEnvelope ble_start{};
  ble_start.device_id = 7U;
  ble_start.principal_id = 1U;
  ble_start.transport = calibration::CalibrationTransport::Ble;
  ble_start.target = calibration::CalibrationTarget::Mq135Gas;
  ble_start.request_id = 101U;
  ble_start.expires_at_us = f.clock.now + 10000000ULL;
  ble_start.operation = calibration::CalibrationOperation::Start;
  ble_start.normalized_mode = true;
  calibration::CalibrationResultEnvelope result{};
  TEST_ASSERT_TRUE(f.controller.execute(ble_start, f.clock.now, result));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::CalibrationCommandStatus::Accepted),
      static_cast<uint8_t>(result.status));
  TEST_ASSERT_TRUE(f.controller.active());

  calibration::CalibrationCommandEnvelope competing = ble_start;
  competing.transport = calibration::CalibrationTransport::Https;
  competing.request_id = 102U;
  competing.expires_at_us = f.clock.now + 10000000ULL;
  TEST_ASSERT_FALSE(f.controller.execute(competing, f.clock.now, result));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::CalibrationCommandStatus::SessionBusy),
      static_cast<uint8_t>(result.status));

  f.clock.now += calibration::CalibrationSession::kSessionTimeoutUs + 1ULL;
  f.controller.poll(f.clock.now);
  TEST_ASSERT_FALSE(f.controller.active());

  competing.request_id = 103U;
  competing.expires_at_us = f.clock.now + 10000000ULL;
  TEST_ASSERT_TRUE(f.controller.execute(competing, f.clock.now, result));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::CalibrationCommandStatus::Accepted),
      static_cast<uint8_t>(result.status));
  TEST_ASSERT_TRUE(f.controller.active());
}

void test_normalized_https_commit_replays_durably_after_controller_recreation() {
  Fixture f{};
  constexpr uint64_t kDeadline = 10000000ULL;
  network::DeviceControlResult result{};
  network::DeviceControlCommand command{};

  TEST_ASSERT_TRUE(make_https_command("normalized-start", "start", "{}",
                                     kDeadline, command));
  TEST_ASSERT_TRUE(f.https.execute(command, f.clock.now, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::Accepted),
                        static_cast<int>(result.status));

  constexpr std::string_view kCriteria =
      "{\"response_mode\":\"normalized\",\"samples\":3,\"min_duration_ms\":2,"
      "\"max_age_ms\":1000,\"max_variation_milli_percent\":5000,"
      "\"circuit_mv\":5000,\"divider_top_ohm\":10000,"
      "\"divider_bottom_ohm\":15000,\"topology\":1,"
      "\"hardware_confirmed\":1,\"reference_confirmed\":1,"
      "\"hardware_identity\":\"SYNTHETIC\","
      "\"reference_environment\":\"SYNTHETIC\","
      "\"reference_kind\":\"clean_air_confirmed\","
      "\"preparation_confirmed\":1,"
      "\"preparation_method\":\"operator_declared\","
      "\"heating_duration_us\":180000000000,"
      "\"reference_temperature_centi_c\":2537,"
      "\"reference_humidity_centi_pct\":4100}";
  TEST_ASSERT_TRUE(make_https_command("normalized-criteria", "set_criteria",
                                     kCriteria, kDeadline, command));
  TEST_ASSERT_TRUE(f.https.execute(command, f.clock.now, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::Accepted),
                        static_cast<int>(result.status));

  for (unsigned index = 0U; index < 3U; ++index) {
    f.next();
    f.set_sample(2308U, 2308.0F);
    const std::string command_id =
        "normalized-capture-" + std::to_string(index);
    TEST_ASSERT_TRUE(make_https_command(command_id, "capture",
                                       "{\"anchor\":\"reference\"}",
                                       kDeadline, command));
    TEST_ASSERT_TRUE(f.https.execute(command, f.clock.now, result));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::Capturing),
                          static_cast<int>(result.status));
  }

  TEST_ASSERT_TRUE(make_https_command("normalized-validate", "validate", "{}",
                                     kDeadline, command));
  TEST_ASSERT_TRUE(f.https.execute(command, f.clock.now, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::Validated),
                        static_cast<int>(result.status));
  TEST_ASSERT_TRUE(make_https_command("normalized-preview", "preview", "{}",
                                     kDeadline, command));
  TEST_ASSERT_TRUE(f.https.execute(command, f.clock.now, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::PreviewReady),
                        static_cast<int>(result.status));

  calibration::CalibrationProfileRef expected{};
  TEST_ASSERT_TRUE(f.router.proposed_profile(expected));
  TEST_ASSERT_TRUE(expected.present);
  TEST_ASSERT_TRUE(expected.version > 0U);
  const std::string malformed_commit_payload =
      "{\"expected_profile_hash\":\"sha256:" + hex(expected.hash) +
      "\",\"expected_profile_version\":" +
      std::to_string(expected.version) + "}";
  acquisition::ActiveProfileSnapshot before_malformed{};
  TEST_ASSERT_TRUE(f.profiles.snapshot(acquisition::kMq135GasKey,
                                       before_malformed));
  TEST_ASSERT_TRUE(make_https_command("normalized-commit", "commit",
                                     malformed_commit_payload, kDeadline, command));
  TEST_ASSERT_FALSE(f.https.execute(command, f.clock.now, result));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::Failed),
                        static_cast<int>(result.status));
  TEST_ASSERT_TRUE(f.controller.active());
  acquisition::ActiveProfileSnapshot after_malformed{};
  TEST_ASSERT_TRUE(f.profiles.snapshot(acquisition::kMq135GasKey,
                                       after_malformed));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(before_malformed.present),
                        static_cast<int>(after_malformed.present));
  if (before_malformed.present) {
    TEST_ASSERT_EQUAL_UINT64(before_malformed.ref.profile_version,
                             after_malformed.ref.profile_version);
    TEST_ASSERT_EQUAL_MEMORY(before_malformed.ref.hash.data(),
                             after_malformed.ref.hash.data(),
                             before_malformed.ref.hash.size());
  }

  const std::string commit_payload =
      "{\"expected_profile_hash\":\"sha256:" + hex(expected.hash) +
      "\",\"expected_profile_version\":\"" +
      std::to_string(expected.version) + "\"}";
  TEST_ASSERT_TRUE(make_https_command("normalized-commit", "commit",
                                     commit_payload, kDeadline, command));
  const auto commit_command = command;
  const bool commit_ok = f.https.execute(command, f.clock.now, result);
  std::printf("DBG https_commit ok=%d status=%u active=%d payload=%.*s\\n",
              static_cast<int>(commit_ok), static_cast<unsigned>(result.status),
              static_cast<int>(f.controller.active()),
              static_cast<int>(result.payload_size), result.payload.data());
  TEST_ASSERT_TRUE(commit_ok);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::Committed),
                        static_cast<int>(result.status));
  const auto original_result = result;

  acquisition::ActiveProfileSnapshot before_replay{};
  TEST_ASSERT_TRUE(f.profiles.snapshot(acquisition::kMq135GasKey,
                                       before_replay));
  TEST_ASSERT_TRUE(before_replay.present);

  calibration::CalibrationTransportController restarted_controller{
      f.router, 7U, &f.auth, &f.replay_store};
  f.router.set_calibration_transport_controller(&restarted_controller);
  ApplicationCalibrationExecutor restarted_https{f.router,
                                                  restarted_controller};
  network::DeviceControlResult replayed{};
  TEST_ASSERT_TRUE(restarted_https.execute(commit_command, f.clock.now + 1U,
                                           replayed));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(network::DeviceControlStatus::Committed),
                        static_cast<int>(replayed.status));
  TEST_ASSERT_EQUAL_UINT(original_result.payload_size, replayed.payload_size);
  TEST_ASSERT_EQUAL_MEMORY(original_result.payload.data(), replayed.payload.data(),
                           original_result.payload_size);
  TEST_ASSERT_FALSE(restarted_controller.active());

  acquisition::ActiveProfileSnapshot after_replay{};
  TEST_ASSERT_TRUE(f.profiles.snapshot(acquisition::kMq135GasKey, after_replay));
  TEST_ASSERT_TRUE(after_replay.present);
  TEST_ASSERT_EQUAL_UINT64(before_replay.ref.profile_version,
                           after_replay.ref.profile_version);
  TEST_ASSERT_EQUAL_MEMORY(before_replay.ref.hash.data(),
                           after_replay.ref.hash.data(),
                           before_replay.ref.hash.size());
}

// The estimator's strict domain status must survive the real MQG2 serializer's
// double-to-decimal conversion at both adjacent representable outside values.
void test_serializer_preserves_adjacent_out_of_domain_ratios() {
  Fixture f{};
  const auto emit = [&f](double ratio) {
    const auto estimate = acquisition::mq135_nh3_curve_estimate(
        acquisition::kMq135Nh3Curve, ratio);
    acquisition::Mq135NormalizedDiagnostic d{};
    d.present = true;
    d.q_present = true;
    d.q = ratio;
    d.q0_present = true;
    d.q0 = 1.0F;
    d.ratio_present = true;
    d.ratio = ratio;
    d.baseline_valid = true;
    d.reference_kind = acquisition::Mq135ReferenceKind::CleanAirConfirmed;
    d.curve_present = true;
    d.curve_id = acquisition::kMq135Nh3CurveId;
    d.curve_version = acquisition::kMq135Nh3CurveVersion;
    d.curve_source_sha256 = acquisition::kMq135Nh3CurveSourceSha256;
    const char* status =
        estimate.status == acquisition::Mq135Nh3CurveStatus::RatioAboveDomain
            ? "BELOW_RANGE"
        : estimate.status == acquisition::Mq135Nh3CurveStatus::RatioBelowDomain
            ? "ABOVE_RANGE"
        : estimate.available() ? "IN_RANGE" : "INVALID_RATIO";
    std::copy_n(status, std::char_traits<char>::length(status), d.nh3_status.begin());
    if (estimate.available()) {
      d.ppm_present = true;
      d.ppm = estimate.ppm;
    }
    maintenance::MaintenanceResponse out{};
    TEST_ASSERT_TRUE(f.router.append_mq135_normalized_result(out, d));
    return std::string(out.view());
  };

  const double above = std::nextafter(
      acquisition::kMq135Nh3CurveMaxRatio,
      std::numeric_limits<double>::infinity());
  const std::string above_json = emit(above);
  TEST_ASSERT_TRUE(contains(above_json, "\"status\":\"BELOW_RANGE\""));
  TEST_ASSERT_TRUE(contains(above_json, "\"ppm\":null"));
  TEST_ASSERT_TRUE(number_after(above_json, "ratio") >
                   acquisition::kMq135Nh3CurveMaxRatio);

  const double below = std::nextafter(
      acquisition::kMq135Nh3CurveMinRatio,
      -std::numeric_limits<double>::infinity());
  const std::string below_json = emit(below);
  TEST_ASSERT_TRUE(contains(below_json, "\"status\":\"ABOVE_RANGE\""));
  TEST_ASSERT_TRUE(contains(below_json, "\"ppm\":null"));
  TEST_ASSERT_TRUE(number_after(below_json, "ratio") <
                   acquisition::kMq135Nh3CurveMinRatio);

  const std::string minimum = emit(acquisition::kMq135Nh3CurveMinRatio);
  TEST_ASSERT_TRUE(contains(minimum, "\"status\":\"IN_RANGE\""));
  TEST_ASSERT_FALSE(contains(minimum, "\"ppm\":null"));
  TEST_ASSERT_TRUE(number_after(minimum, "ratio") ==
                   acquisition::kMq135Nh3CurveMinRatio);
  const std::string maximum = emit(acquisition::kMq135Nh3CurveMaxRatio);
  TEST_ASSERT_TRUE(contains(maximum, "\"status\":\"IN_RANGE\""));
  TEST_ASSERT_FALSE(contains(maximum, "\"ppm\":null"));
  TEST_ASSERT_TRUE(number_after(maximum, "ratio") ==
                   acquisition::kMq135Nh3CurveMaxRatio);
}

// Measured transport bounds. The valid MQG2 bytes come from the real session,
// profile manager, controller and serializer; every BLE notification is then
// exercised through the production R1 fragmenter at the 23-byte minimum MTU.
void test_worst_case_bounds_fit_each_transport() {
  Fixture f{};
  const std::string worst = f.valid_worst_case_json();
  TEST_ASSERT_FALSE(worst.empty());
  TEST_ASSERT_TRUE(contains(worst, "\"sequence\":\"18446744073709551615\""));
  TEST_ASSERT_TRUE(contains(worst, "\"max_age_us\":\"9223372036854775000\""));
  TEST_ASSERT_TRUE(contains(worst, "\"heating_duration_us\":\"9223372036854775807\""));
  TEST_ASSERT_TRUE(contains(worst, "SYNTHETIC-" + std::string(21U, 'H')));
  TEST_ASSERT_TRUE(contains(worst, "REFERENCE-" + std::string(21U, 'R')));
  TEST_ASSERT_TRUE(contains(worst, "\"status\":\"IN_RANGE\""));
  const std::size_t payload = worst.size();

  // Exact USB response from the same active diagnostic, not an inferred bound.
  const std::string usb = f.raw_result_json();
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(worst.size()),
                         static_cast<unsigned>(usb.size()));
  TEST_ASSERT_EQUAL_MEMORY(worst.data(), usb.data(), worst.size());
  TEST_ASSERT_TRUE(payload < maintenance::kMaxResponseBytes);

  // Encode the actual producer bytes as the HTTPS device-control result object.
  network::DeviceControlResult result{};
  result.status = network::DeviceControlStatus::Committed;
  TEST_ASSERT_TRUE(result.command_id.assign(std::string(64U, 'C')));
  TEST_ASSERT_TRUE(result.session_id.assign(std::string(64U, 'S')));
  result.payload_size = payload;
  std::copy(worst.begin(), worst.end(), result.payload.begin());
  network::DeviceControlWireBody https{};
  TEST_ASSERT_TRUE(network::encode_device_control_result(result, https));
  TEST_ASSERT_TRUE(https.size <= network::kMaxDeviceControlWireBytes);
  TEST_ASSERT_TRUE(payload < network::kMaxDeviceControlPayloadBytes);
  TEST_ASSERT_TRUE(https.view().find(worst) != std::string_view::npos);

  // Route the same actual diagnostic through CAL1, then through the real BLE
  // session/R1 chunker. A 23-byte ATT MTU admits at most 20 bytes per notify.
  const std::string expected_cal1 = f.evidence_json();
  TEST_ASSERT_FALSE(expected_cal1.empty());
  QuietWifiBackend wifi{};
  provisioning::BleProvisioningSession session{wifi, f.clock};
  maintenance::CommandRouterBleCalibrationBackend calibration_backend{
      f.router, f.clock, &f.controller};
  session.set_device_id(7U);
  session.set_calibration_backend(&calibration_backend);
  session.on_connect(f.clock.now);
  provisioning::BleResponse response{};
  constexpr std::array<std::string_view, 3U> kFrames{{
      "F1|42|0|3|CAL E", "F1|42|1|3|VIDENCE ",
      "F1|42|2|3|mq135_gas"}};
  for (const auto frame : kFrames) {
    TEST_ASSERT_TRUE(frame.size() <= 20U);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(provisioning::BleParseStatus::Accepted),
        static_cast<uint8_t>(session.receive(frame, response)));
  }
  TEST_ASSERT_TRUE(response.notify);
  const uint16_t chunks = response.observation.chunk_total;
  TEST_ASSERT_TRUE(chunks > 1U);
  std::size_t assembled_size{0U};
  std::size_t maximum_packet{0U};
  std::string assembled;
  for (uint16_t expected_chunk = 0U; expected_chunk < chunks; ++expected_chunk) {
    TEST_ASSERT_TRUE(response.notify);
    TEST_ASSERT_TRUE(response.size <= 20U);
    maximum_packet = std::max(maximum_packet, response.size);
    const std::string packet(response.bytes.data(), response.size);
    TEST_ASSERT_TRUE(packet.find("R1|42|") == 0U);
    TEST_ASSERT_EQUAL_UINT(expected_chunk, response.observation.chunk_index);
    TEST_ASSERT_EQUAL_UINT(chunks, response.observation.chunk_total);
    const auto p1 = packet.find('|');
    const auto p2 = packet.find('|', p1 + 1U);
    const auto p3 = packet.find('|', p2 + 1U);
    const auto p4 = packet.find('|', p3 + 1U);
    TEST_ASSERT_TRUE(p4 != std::string::npos);
    const std::string_view bytes{packet.data() + p4 + 1U,
                                 packet.size() - p4 - 1U};
    assembled.append(bytes.data(), bytes.size());
    assembled_size += bytes.size();
    session.observe_notification(response, true);
    if (expected_chunk + 1U < chunks) session.poll(response);
  }
  TEST_ASSERT_EQUAL_UINT(expected_cal1.size(), assembled_size);
  TEST_ASSERT_EQUAL_UINT(expected_cal1.size(), assembled.size());
  TEST_ASSERT_EQUAL_MEMORY(expected_cal1.data(), assembled.data(), assembled.size());
  TEST_ASSERT_TRUE(payload < maintenance::kMaxResponseBytes);

  const std::string stress = Fixture{}.serializer_stress_json();
  TEST_ASSERT_FALSE(stress.empty());
  std::printf("MQG2_TRANSPORT usb_payload=%u usb_capacity=%u https_body=%u https_capacity=%u ble_cal1=%u mtu=23 max_notify=%u chunks=%u stress_bytes=%u\n",
              static_cast<unsigned>(payload),
              static_cast<unsigned>(maintenance::kMaxResponseBytes),
              static_cast<unsigned>(https.size),
              static_cast<unsigned>(network::kMaxDeviceControlWireBytes),
              static_cast<unsigned>(assembled_size),
              static_cast<unsigned>(maximum_packet),
              static_cast<unsigned>(chunks),
              static_cast<unsigned>(stress.size()));
  if (!fixture_dir().empty()) {
    const std::string metrics =
        "{\"usb_payload_bytes\":" + std::to_string(payload) +
        ",\"usb_capacity_bytes\":" +
        std::to_string(maintenance::kMaxResponseBytes) +
        ",\"https_payload_bytes\":" + std::to_string(payload) +
        ",\"https_body_bytes\":" + std::to_string(https.size) +
        ",\"https_capacity_bytes\":" +
        std::to_string(network::kMaxDeviceControlWireBytes) +
        ",\"ble_min_mtu_bytes\":23,\"ble_att_payload_bytes\":20"
        ",\"ble_cal1_logical_bytes\":" + std::to_string(assembled_size) +
        ",\"ble_max_notification_bytes\":" +
        std::to_string(maximum_packet) +
        ",\"ble_response_chunks\":" + std::to_string(chunks) + "}";
    TEST_ASSERT_TRUE(write_file(fixture_path("transport-measurements"), metrics));
  }
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(payload),
                         static_cast<unsigned>(freeze("worst-case-valid", worst)));
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(stress.size()),
                         static_cast<unsigned>(freeze(
                             "negative/serializer-stress-invalid", stress)));
}

// Wire criteria rejections: unknown key, duplicate key, legacy-only key and an
// unconfirmed clean-air claim must all be rejected before any mutation.
void test_normalized_criteria_wire_rejections() {
  calibration::Mq135NormalizedParameters p{};
  TEST_ASSERT_TRUE(p.arguments(kNormalizedParameters));
  TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::Mq135NormalizedParameters::Status::Complete),
      static_cast<uint8_t>(p.status()));

  calibration::Mq135NormalizedParameters duplicate{};
  TEST_ASSERT_FALSE(duplicate.arguments(
      std::string(kNormalizedParameters) + " samples=4"));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::Mq135NormalizedParameters::Status::Duplicate),
      static_cast<uint8_t>(duplicate.status()));

  calibration::Mq135NormalizedParameters unknown{};
  TEST_ASSERT_FALSE(unknown.arguments(
      std::string(kNormalizedParameters) + " r0_ohm=1"));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::Mq135NormalizedParameters::Status::UnknownKey),
      static_cast<uint8_t>(unknown.status()));

  // module_load_ohm is legacy-only and rejected in normalized mode.
  calibration::Mq135NormalizedParameters mixed{};
  mixed.set("response_mode", "normalized");
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::Mq135NormalizedParameters::Status::MixedMode),
      static_cast<uint8_t>(mixed.set("module_load_ohm", "10000")));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::Mq135NormalizedParameters::Status::MixedMode),
      static_cast<uint8_t>(mixed.status()));

  // A normalized-only key before its discriminator is a mixed-mode request.
  calibration::Mq135NormalizedParameters unordered{};
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(calibration::Mq135NormalizedParameters::Status::MixedMode),
      static_cast<uint8_t>(unordered.set("reference_kind", "operational")));

  // Clean air requires >48h documented operator-declared preheat.
  Fixture f{};
  TEST_ASSERT_TRUE(f.line("maintenance enter"));
  TEST_ASSERT_TRUE(f.line("calibration start mq135_gas"));
  std::string short_preheat{kNormalizedParameters};
  short_preheat.replace(short_preheat.find("heating_duration_us=180000000000"),
                        std::string("heating_duration_us=180000000000").size(),
                        "heating_duration_us=172800000000");
  std::string values = short_preheat;
  bool configured = true;
  while (!values.empty()) {
    auto end = values.find(' ');
    auto token = values.substr(0, end);
    auto at = token.find('=');
    configured = f.line("calibration criteria " + token.substr(0, at) + " " +
                        token.substr(at + 1));
    if (!configured) break;
    if (end == std::string::npos) break;
    values.erase(0, end + 1);
  }
  TEST_ASSERT_FALSE(configured);  // exactly 48 h is not "strictly greater"
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_normalized_session_emits_complete_mqg2_groups);
  RUN_TEST(test_operational_baseline_keeps_ratio_and_nulls_ppm);
  RUN_TEST(test_both_out_of_range_directions_keep_ratio_and_null_ppm);
  RUN_TEST(test_raw_absent_sample_is_not_admitted);
  RUN_TEST(test_normalized_source_identity_sequence_clock_and_profile_guards);
  RUN_TEST(test_normalized_adc_counts_method_settings_and_saturation_guards);
  RUN_TEST(test_unavailable_calibration_is_explicit_not_a_fallback);
  RUN_TEST(test_source_age_boundary_is_inclusive_and_overrun_is_stale);
  RUN_TEST(test_stale_source_nulls_current_quantities_but_keeps_q0);
  RUN_TEST(test_legacy_mqg1_path_remains_when_no_normalized_mode_selected);
  RUN_TEST(test_normalized_evidence_replay_keeps_original_snapshot);
  RUN_TEST(test_normalized_controller_shares_owner_and_expires);
  RUN_TEST(test_normalized_https_commit_replays_durably_after_controller_recreation);
  RUN_TEST(test_serializer_preserves_adjacent_out_of_domain_ratios);
  RUN_TEST(test_worst_case_bounds_fit_each_transport);
  RUN_TEST(test_normalized_criteria_wire_rejections);
  return UNITY_END();
}
