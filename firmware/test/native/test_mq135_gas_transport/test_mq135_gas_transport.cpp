#include <string>
#include <unity.h>
#include "../test_debug_admission/admission_fixtures.h"
#include "maintenance/ble_calibration_backend.h"
#include "calibration/mq135_reference_parameters.h"
#define ARDUINO
#include "../../../src/app/device_control_executor.cpp"
#undef ARDUINO
using namespace sigap;
void setUp() {}
void tearDown() {}
namespace {
const std::string parameters = "samples=3 min_duration_ms=2 max_age_ms=1 max_variation_milli_percent=10000 circuit_mv=5000 module_load_ohm=10000 divider_top_ohm=10000 divider_bottom_ohm=15000 topology=1 hardware_confirmed=1 preparation_confirmed=1 reference_confirmed=1 reference_temperature_centi_c=2500 reference_humidity_centi_pct=5000 heating_duration_us=10000 hardware_identity=SYNTHETIC reference_environment=SYNTHETIC";
struct Clock : maintenance::ICalibrationClock {
  uint64_t now{100}; uint64_t now_us() const noexcept override { return now; }
};
struct Authorizer : calibration::ICalibrationTransportAuthorizer {
  bool allow{true};
  bool authorize(const calibration::CalibrationCommandEnvelope&) const noexcept override { return allow; }
};
struct Evidence : calibration::ICalibrationEvidenceSource {
  calibration::MqTrial value{};
  bool latest(calibration::CalibrationTarget target, uint64_t after,
              calibration::TrialObservation& out, uint64_t& sequence) const noexcept override {
    if (target != calibration::CalibrationTarget::Mq135Gas || value.source_sequence <= after) return false;
    out = value; sequence = value.source_sequence; return true;
  }
  bool diagnostic(calibration::CalibrationTarget, calibration::DiagnosticEvidence& out) const noexcept override {
    out.available = true; out.sensor = acquisition::SensorId::Mq135;
    out.quality = value.quality; return true;
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
  Evidence evidence{}; Clock clock{}; Authorizer auth{};
  maintenance::CommandRouter router{config, secrets, &profiles, &evidence, nullptr, nullptr, &boot};
  calibration::CalibrationTransportController controller{router, 7, &auth};
  maintenance::CommandRouterBleCalibrationBackend ble{router, clock, &controller};
  ApplicationCalibrationExecutor https{router, controller};
  acquisition::EncodedProfile original_adc{};
  Fixture() {
    (void)config.begin(); boot.present = true; boot.bytes.fill(1);
    acquisition::MqAdcProfile a{};
    a.manifest.kind = acquisition::ProfileKind::Adc; a.manifest.target_id = 8;
    a.manifest.profile_version = 1; a.manifest.validity = acquisition::ProfileValidity::TestOnly;
    a.manifest.provenance.present = true; a.manifest.provenance.evidence_count = 3;
    a.reference_mv = a.full_scale_counts = 3000; a.divider_numerator = 3; a.divider_denominator = 5;
    TEST_ASSERT_TRUE(codec.encode(a, original_adc));
    TEST_ASSERT_TRUE(profiles.stage_import(acquisition::ProfileTarget::Mq135, original_adc));
    TEST_ASSERT_TRUE(profiles.preview(acquisition::ProfileTarget::Mq135));
    TEST_ASSERT_TRUE(profiles.commit(acquisition::ProfileTarget::Mq135));
    TEST_ASSERT_TRUE(profiles.load(acquisition::kMq135GasKey));
    TEST_ASSERT_TRUE(codec.decode(original_adc, a));
    evidence.value.source_profile = a.ref(); evidence.value.source_boot_id = boot;
    evidence.value.raw_present = evidence.value.identity_valid = true;
    evidence.value.adc_counts = 1500; evidence.value.quality = acquisition::SampleQuality::Ready;
    router.set_calibration_transport_controller(&controller);
  }
  void next() {
    clock.now += 1000; ++evidence.value.source_sequence; evidence.value.observed_at_us = clock.now;
  }
  bool line(const std::string& text, std::string* output = nullptr) {
    maintenance::MaintenanceParser parser{}; maintenance::ParseEvent event{};
    for (char byte : text) (void)parser.consume(byte, event);
    if (!parser.consume('\n', event) || event.status != maintenance::CommandStatus::Accepted) return false;
    maintenance::MaintenanceResponse response{};
    const auto result = router.execute(event.command, clock.now, response);
    if (output) output->assign(response.view());
    return result == maintenance::CommandStatus::Accepted;
  }
  bool radio(const std::string& text, std::string* output = nullptr) {
    provisioning::BleCommand parsed{};
    if (provisioning::BleCommandParser::parse(text, parsed) != provisioning::BleParseStatus::Accepted) return false;
    char buffer[2048]{}; std::size_t size{};
    const bool ok = ble.execute(text, buffer, sizeof(buffer), size);
    if (output) output->assign(buffer, size); return ok;
  }
  void committed() {
    acquisition::EncodedProfile actual{}; acquisition::Mq135GasProfile p{};
    TEST_ASSERT_TRUE(profiles.export_active(acquisition::kMq135GasKey, actual));
    TEST_ASSERT_TRUE(codec.decode(actual, p)); TEST_ASSERT_TRUE(p.valid());
    TEST_ASSERT_FLOAT_WITHIN(.01, 50000.0/7.0, p.r0_ohm);
    TEST_ASSERT_EQUAL(acquisition::ProfileValidity::TestOnly, p.manifest.validity);
    TEST_ASSERT_TRUE(profiles.export_active(acquisition::ProfileTarget::Mq135, actual));
    TEST_ASSERT_EQUAL_MEMORY(original_adc.bytes.data(), actual.bytes.data(), actual.size);
  }
};
std::string hex(const config::ConfigHash& hash) {
  const char digits[] = "0123456789abcdef"; std::string text;
  for (auto b : hash) { text += digits[b >> 4]; text += digits[b & 15]; } return text;
}
void test_ble_parser_accepts_full_reference_and_extended_fragment_bounds() {
  provisioning::BleCommand command{};
  const auto line = "CAL CRITERIA mq135_gas " + parameters;
  TEST_ASSERT_TRUE(line.size() > 254);
  TEST_ASSERT_EQUAL(provisioning::BleParseStatus::Accepted, provisioning::BleCommandParser::parse(line, command));
  TEST_ASSERT_TRUE(command.calibration_arguments_size > 192);
  provisioning::BleWireFrame frame{};
  TEST_ASSERT_EQUAL(provisioning::BleParseStatus::Accepted, provisioning::BleCommandParser::decode_frame("F1|42|511|512|x", frame));
  TEST_ASSERT_EQUAL(provisioning::BleParseStatus::Invalid, provisioning::BleCommandParser::decode_frame("F1|42|512|513|x", frame));
}
void test_usb_gas_reference_preview_commit_and_unrelated_channels() {
  Fixture f{}; TEST_ASSERT_TRUE(f.line("maintenance enter"));
  TEST_ASSERT_TRUE(f.line("calibration start mq135_gas"));
  TEST_ASSERT_FALSE(f.router.is_calibrating(acquisition::SensorId::Hx711Organic));
  TEST_ASSERT_FALSE(f.router.is_calibrating(acquisition::SensorId::Mq135));
  std::string values = parameters;
  while (!values.empty()) {
    auto end = values.find(' '); auto token = values.substr(0, end); auto at = token.find('=');
    TEST_ASSERT_TRUE_MESSAGE(f.line("calibration criteria " + token.substr(0, at) + " " + token.substr(at + 1)), token.c_str());
    if (end == std::string::npos) break; values.erase(0, end + 1);
  }
  for (unsigned i = 0; i < 3; ++i) { f.next(); TEST_ASSERT_TRUE(f.line("calibration capture reference")); }
  TEST_ASSERT_TRUE(f.line("calibration validate"));
  std::string output; TEST_ASSERT_TRUE(f.line("calibration preview", &output));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, output.find("r0_ohm="));
  TEST_ASSERT_TRUE(f.line("calibration commit")); f.committed();
  TEST_ASSERT_TRUE(f.line("profile export mq135_gas", &output));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, output.find("PROFILE BEGIN"));
}
void test_ble_gas_reference_uses_same_controller_and_versioned_readback() {
  Fixture f{}; TEST_ASSERT_TRUE(f.radio("CAL START mq135_gas"));
  TEST_ASSERT_TRUE(f.radio("CAL CRITERIA mq135_gas " + parameters));
  for (unsigned i = 0; i < 3; ++i) { f.next(); TEST_ASSERT_TRUE(f.radio("CAL CAPTURE mq135_gas reference")); }
  TEST_ASSERT_TRUE(f.radio("CAL VALIDATE mq135_gas"));
  TEST_ASSERT_TRUE(f.radio("CAL PREVIEW mq135_gas"));
  calibration::CalibrationProfileRef ref{}; TEST_ASSERT_TRUE(f.router.proposed_profile(ref));
  TEST_ASSERT_TRUE(f.radio("CAL COMMIT mq135_gas expected_profile_version=" + std::to_string(ref.version) + " expected_profile_hash=sha256:" + hex(ref.hash)));
  f.committed(); std::string output;
  TEST_ASSERT_TRUE(f.radio("CAL EVIDENCE mq135_gas", &output));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, output.find("MQG1"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, output.find("\"nh3_ppm\":null"));
  TEST_ASSERT_TRUE(f.radio("CAL ACTIVE_PROFILE mq135_gas", &output));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, output.find("profile_version"));
}
network::DeviceControlCommand http_command(const Fixture& f, std::string operation,
                                           const std::string& payload, unsigned id) {
  network::DeviceControlCommand cmd{};
  cmd.command_id.assign("gas-http-" + std::to_string(id));
  cmd.device_id.assign("0000000000000007"); cmd.operator_id.assign("synthetic-operator");
  cmd.session_id.assign("synthetic-gas-session"); cmd.sensor_id.assign("mq135_gas"); cmd.operation.assign(operation);
  std::copy(payload.begin(), payload.end(), cmd.payload.begin()); cmd.payload_size = payload.size();
  cmd.request_hash.assign("sha256:" + hex(telemetry_contract::sha256(payload + std::to_string(id))));
  cmd.expires_at_us = f.clock.now + 100000000; return cmd;
}
std::string json_parameters() {
  std::string out = "{"; std::string values = parameters;
  while (!values.empty()) {
    auto end = values.find(' '); auto token = values.substr(0, end); auto at = token.find('=');
    const auto key = token.substr(0, at); const auto value = token.substr(at + 1);
    if (out.size() > 1) out += ',';
    out += '\"' + key + "\":";
    out += key == "hardware_identity" || key == "reference_environment" ? '\"' + value + '\"' : value;
    if (end == std::string::npos) break; values.erase(0, end + 1);
  }
  return out + '}';
}
void test_https_executes_gas_reference_and_replays_commit_without_reactivation() {
  Fixture f{}; network::DeviceControlResult result{}; unsigned id = 1;
  auto step = [&](const char* operation, const std::string& payload) {
    const auto cmd = http_command(f, operation, payload, id++);
    return f.https.execute(cmd, f.clock.now, result);
  };
  TEST_ASSERT_TRUE(step("start", "{}"));
  TEST_ASSERT_TRUE(step("set_criteria", json_parameters()));
  for (unsigned i = 0; i < 3; ++i) { f.next(); TEST_ASSERT_TRUE(step("capture", "{\"anchor\":\"reference\"}")); }
  TEST_ASSERT_TRUE(step("validate", "{}")); TEST_ASSERT_TRUE(step("preview", "{}"));
  calibration::CalibrationProfileRef ref{}; TEST_ASSERT_TRUE(f.router.proposed_profile(ref));
  const auto payload = "{\"expected_profile_version\":\"" + std::to_string(ref.version) + "\",\"expected_profile_hash\":\"sha256:" + hex(ref.hash) + "\"}";
  const auto commit = http_command(f, "commit", payload, id++);
  TEST_ASSERT_TRUE(f.https.execute(commit, f.clock.now, result)); f.committed();
  const auto generation = f.profiles.generation();
  TEST_ASSERT_TRUE(f.https.execute(commit, f.clock.now, result));
  TEST_ASSERT_EQUAL_UINT32(generation, f.profiles.generation());
  TEST_ASSERT_TRUE(step("evidence", "{}"));
  std::string text(reinterpret_cast<const char*>(result.payload.data()), result.payload_size);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("\"schema\":\"MQG1\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("\"rs_over_r0\":1"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("\"nh3_ppm\":null"));
  std::printf("MQ135_CONTRACT_FIXTURE=%s\n", text.c_str());
  const auto read = http_command(f, "evidence", "{}", id++);
  TEST_ASSERT_TRUE(f.https.execute(read, f.clock.now, result));
  const std::string cached(reinterpret_cast<const char*>(result.payload.data()), result.payload_size);
  f.next(); f.evidence.value.adc_counts = 1800;
  TEST_ASSERT_TRUE(f.https.execute(read, f.clock.now, result));
  TEST_ASSERT_EQUAL_STRING(cached.c_str(), std::string(reinterpret_cast<const char*>(result.payload.data()), result.payload_size).c_str());
}
void seed_reference(Fixture& f) {
  TEST_ASSERT_TRUE(f.radio("CAL START mq135_gas"));
  TEST_ASSERT_TRUE(f.radio("CAL CRITERIA mq135_gas " + parameters));
  for (unsigned i = 0; i < 3; ++i) { f.next(); TEST_ASSERT_TRUE(f.radio("CAL CAPTURE mq135_gas reference")); }
  TEST_ASSERT_TRUE(f.radio("CAL VALIDATE mq135_gas")); TEST_ASSERT_TRUE(f.radio("CAL PREVIEW mq135_gas"));
  calibration::CalibrationProfileRef ref{}; TEST_ASSERT_TRUE(f.router.proposed_profile(ref));
  TEST_ASSERT_TRUE(f.radio("CAL COMMIT mq135_gas expected_profile_version=" + std::to_string(ref.version) + " expected_profile_hash=sha256:" + hex(ref.hash)));
}
void test_stale_missing_raw_and_changed_identity_never_reuse_last_good_ratio() {
  Fixture f{}; seed_reference(f);
  acquisition::Mq135Diagnostic d{};
  TEST_ASSERT_TRUE(f.router.gas_diagnostic(calibration::CalibrationTarget::Mq135Gas, f.clock.now, d));
  TEST_ASSERT_TRUE(d.ratio_present);
  f.clock.now += 2000;
  TEST_ASSERT_TRUE(f.router.gas_diagnostic(calibration::CalibrationTarget::Mq135Gas, f.clock.now, d));
  TEST_ASSERT_FALSE(d.ratio_present); TEST_ASSERT_FALSE(d.rs_present); TEST_ASSERT_TRUE(d.raw_present);
  TEST_ASSERT_EQUAL_STRING("STALE", d.quality.data());
  f.next(); f.evidence.value.raw_present = false;
  TEST_ASSERT_TRUE(f.router.gas_diagnostic(calibration::CalibrationTarget::Mq135Gas, f.clock.now, d));
  TEST_ASSERT_FALSE(d.ratio_present); TEST_ASSERT_FALSE(d.rs_present);
  f.evidence.value.raw_present = true; f.evidence.value.source_profile.hash[0] ^= 1;
  TEST_ASSERT_TRUE(f.router.gas_diagnostic(calibration::CalibrationTarget::Mq135Gas, f.clock.now, d));
  TEST_ASSERT_FALSE(d.ratio_present); TEST_ASSERT_EQUAL_STRING("ADC_PROFILE_CHANGED", d.reason.data());
}
void test_history_and_cas_rollback_use_separate_gas_profile_key() {
  Fixture f{}; seed_reference(f);
  acquisition::EncodedProfile encoded{}; acquisition::Mq135GasProfile p{};
  TEST_ASSERT_TRUE(f.profiles.export_active(acquisition::kMq135GasKey, encoded));
  TEST_ASSERT_TRUE(f.codec.decode(encoded, p)); p.manifest.profile_version = 2;
  TEST_ASSERT_TRUE(f.codec.encode(p, encoded));
  TEST_ASSERT_TRUE(f.profiles.stage_import(acquisition::kMq135GasKey, encoded));
  TEST_ASSERT_TRUE(f.profiles.preview(acquisition::kMq135GasKey));
  TEST_ASSERT_TRUE(f.profiles.commit(acquisition::kMq135GasKey));
  std::string history; TEST_ASSERT_TRUE(f.radio("CAL HISTORY mq135_gas", &history));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, history.find("\"previous_profile_version\":\"1\""));
  TEST_ASSERT_TRUE(f.radio("CAL START mq135_gas"));
  TEST_ASSERT_FALSE(f.radio("CAL ROLLBACK mq135_gas"));
  TEST_ASSERT_TRUE(f.radio("CAL ROLLBACK mq135_gas expected_profile_version=2 expected_profile_hash=sha256:" + hex(encoded.hash())));
  TEST_ASSERT_TRUE(f.profiles.export_active(acquisition::kMq135GasKey, encoded));
  TEST_ASSERT_TRUE(f.codec.decode(encoded, p)); TEST_ASSERT_EQUAL_UINT32(1, p.manifest.profile_version);
  f.committed();
}
void test_reference_parameters_reject_unknown_duplicate_and_unconfirmed_inputs() {
  calibration::Mq135ReferenceParameters p{};
  TEST_ASSERT_TRUE(p.arguments(parameters)); TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_FALSE(p.set("r0_ohm", "1")); TEST_ASSERT_FALSE(p.set("samples", "4"));
  calibration::Mq135ReferenceParameters bad{};
  TEST_ASSERT_FALSE(bad.set("hardware_confirmed", "0"));
  TEST_ASSERT_FALSE(bad.set("hardware_identity", "injected;rs_ohm=1"));
  TEST_ASSERT_FALSE(bad.set("min_duration_ms", "9223372036854775807"));
  TEST_ASSERT_FALSE(bad.complete());
}
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ble_parser_accepts_full_reference_and_extended_fragment_bounds);
  RUN_TEST(test_usb_gas_reference_preview_commit_and_unrelated_channels);
  RUN_TEST(test_ble_gas_reference_uses_same_controller_and_versioned_readback);
  RUN_TEST(test_https_executes_gas_reference_and_replays_commit_without_reactivation);
  RUN_TEST(test_stale_missing_raw_and_changed_identity_never_reuse_last_good_ratio);
  RUN_TEST(test_history_and_cas_rollback_use_separate_gas_profile_key);
  RUN_TEST(test_reference_parameters_reject_unknown_duplicate_and_unconfirmed_inputs);
  return UNITY_END();
}
