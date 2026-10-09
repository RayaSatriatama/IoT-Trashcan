#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <string>
#include <string_view>

#include <unity.h>

#include "maintenance/maintenance.h"
#include "maintenance/storage_probe_response.h"
#include <limits>
#include "maintenance/runtime.h"
#include "calibration/calibration_session.h"
#include "acquisition/profile_manager.h"
#include "acquisition/profile_store.h"
#include "config/config_store.h"
#include "config/config_validator.h"
#include "provisioning/secret_store.h"

namespace {

using namespace sigap::maintenance;

constexpr sigap::config::ValidationContext kValidationContext{
    8ULL * 1024ULL * 1024ULL, 0U};

sigap::acquisition::Hx711Profile make_profile(uint32_t version,
                                               float counts_per_kg) {
  sigap::acquisition::Hx711Profile profile{};
  profile.manifest.schema_version = 1U;
  profile.manifest.profile_version = version;
  profile.manifest.target_id = static_cast<uint32_t>(
      sigap::acquisition::ProfileTarget::Hx711Organic);
  profile.manifest.validity = sigap::acquisition::ProfileValidity::Valid;
  profile.tare_offset_counts = 1000;
  profile.counts_per_kg = counts_per_kg;
  return profile;
}

sigap::acquisition::Sht31Profile make_sht_profile(
    uint32_t version, uint8_t address,
    sigap::acquisition::ProfileTarget target) {
  sigap::acquisition::Sht31Profile profile{};
  profile.manifest.schema_version = 1U;
  profile.manifest.profile_version = version;
  profile.manifest.target_id = static_cast<uint32_t>(target);
  profile.manifest.validity = sigap::acquisition::ProfileValidity::Valid;
  profile.address = address;
  profile.temperature_offset_centi_c = 25;
  profile.humidity_offset_centi_pct = -10;
  return profile;
}

std::string hex(const sigap::acquisition::EncodedProfile& encoded) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(encoded.size * 2U);
  for (std::size_t i = 0U; i < encoded.size; ++i) {
    output.push_back(digits[encoded.bytes[i] >> 4U]);
    output.push_back(digits[encoded.bytes[i] & 0x0fU]);
  }
  return output;
}

std::string hash_hex(const sigap::acquisition::EncodedProfile& encoded) {
  static constexpr char digits[] = "0123456789abcdef";
  const auto hash = encoded.hash();
  std::string output;
  output.reserve(hash.size() * 2U);
  for (const uint8_t byte : hash) {
    output.push_back(digits[byte >> 4U]);
    output.push_back(digits[byte & 0x0fU]);
  }
  return output;
}

struct FakeTransport final : IMaintenanceTransport {
  std::string input;
  std::string output;
  std::size_t offset{0};
  bool link{true};

  bool available() noexcept override { return offset < input.size(); }

  int read() noexcept override {
    if (!available()) return -1;
    return static_cast<unsigned char>(input[offset++]);
  }

  bool write(std::string_view value) noexcept override {
    output.append(value.data(), value.size());
    return true;
  }

  bool connected() noexcept override { return link; }
};

struct FakeSink final : IMaintenanceCommandSink {
  std::size_t calls{0};
  std::size_t poll_calls{0};
  std::size_t transport_lost_calls{0};
  MaintenanceCommand last{};

  CommandStatus execute(const MaintenanceCommand& command, uint64_t,
                        MaintenanceResponse& response) noexcept override {
    ++calls;
    last = command;
    response.append("OK ");
    response.append(command_name(command.kind));
    response.append("\n");
    return CommandStatus::Accepted;
  }

  void poll(uint64_t) noexcept override { ++poll_calls; }

  void transport_lost() noexcept override { ++transport_lost_calls; }
};

struct CalibrationEvidenceFake final
    : sigap::calibration::ICalibrationEvidenceSource {
  sigap::calibration::TrialObservation trial{};
  uint64_t sequence{0U};
  float ultrasonic_conversion{0.0F};

  bool latest(sigap::calibration::CalibrationTarget target,
              uint64_t after_sequence,
              sigap::calibration::TrialObservation& output,
              uint64_t& observed_sequence) const noexcept override {
    if (sequence == 0U || sequence <= after_sequence) return false;
    const bool ds_target =
        target == sigap::calibration::CalibrationTarget::Ds18b20B3;
    const bool variant_matches =
        (ds_target && std::holds_alternative<sigap::calibration::Ds18b20Trial>(trial)) ||
        (!ds_target && std::holds_alternative<sigap::calibration::Hx711Trial>(trial));
    if (!variant_matches) return false;
    output = trial;
    observed_sequence = sequence;
    return true;
  }

  bool set_ultrasonic_engineering_conversion(
      sigap::calibration::CalibrationTarget, float value) noexcept override {
    ultrasonic_conversion = value;
    return value > 0.0F;
  }
};

CommandStatus run_router_line(CommandRouter& router, const char* line,
                              uint64_t now_us,
                              MaintenanceResponse& response) {
  MaintenanceParser parser{};
  ParseEvent event{};
  bool emitted{false};
  for (const char character : std::string(line)) {
    emitted = parser.consume(character, event) || emitted;
  }
  TEST_ASSERT_TRUE(emitted);
  response = MaintenanceResponse{};
  return router.execute(event.command, now_us, response);
}

sigap::acquisition::SampleRecord diagnostic_sample(
    sigap::acquisition::SensorId sensor,
    sigap::acquisition::StreamId stream,
    sigap::acquisition::SampleQuality quality,
    sigap::acquisition::SampleReason reason,
    uint64_t sequence, uint64_t monotonic_us) {
  sigap::acquisition::SampleRecord sample{};
  sample.header.sensor = sensor;
  sample.header.stream = stream;
  sample.header.sequence = sequence;
  sample.header.monotonic_us = monotonic_us;
  sample.quality = quality;
  sample.reason = reason;
  return sample;
}

struct FakePasskeyHandoff final
    : sigap::provisioning::ILocalPasskeyHandoff {
  sigap::provisioning::PasskeyDisclosureStatus next{
      sigap::provisioning::PasskeyDisclosureStatus::Ready};
  std::size_t calls{0U};

  sigap::provisioning::PasskeyDisclosureStatus disclose_once(
      char* output, std::size_t capacity) noexcept override {
    ++calls;
    if (next != sigap::provisioning::PasskeyDisclosureStatus::Ready) return next;
    if (output == nullptr || capacity < 7U) {
      return sigap::provisioning::PasskeyDisclosureStatus::Unavailable;
    }
    const char value[] = "654321";
    for (std::size_t i = 0U; i < 7U; ++i) output[i] = value[i];
    next = sigap::provisioning::PasskeyDisclosureStatus::AlreadyDisclosed;
    return sigap::provisioning::PasskeyDisclosureStatus::Ready;
  }
};

void drain(MaintenanceService& service, FakeTransport& transport,
           uint64_t now_us = 100U) {
  while (transport.available()) service.poll(now_us);
}

void test_profile_lifecycle_commands_parse_as_explicit_steps() {
  FakeTransport transport{};
  transport.input = "profile validate hx711_organic\n"
                    "profile preview hx711_organic\n"
                    "profile commit hx711_organic\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  while (transport.available()) service.poll(0U);

  TEST_ASSERT_EQUAL_UINT32(3U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::ProfileCommit),
                        static_cast<int>(sink.last.kind));
  TEST_ASSERT_EQUAL_STRING("hx711_organic", sink.last.target.c_str());
}

void test_storage_sd_probe_command_is_bounded_and_explicit() {
  FakeTransport transport{};
  transport.input = "storage sd test\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);
  drain(service, transport);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::StorageSdTest),
                        static_cast<int>(sink.last.kind));
}

void test_ble_passkey_command_requires_maintenance_and_discloses_once() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  FakePasskeyHandoff handoff{};
  CommandRouter router(config_manager, secrets, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr, &handoff);

  MaintenanceCommand command{};
  command.kind = CommandKind::BlePasskey;
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Rejected),
                        static_cast<int>(router.execute(command, 0U, response)));
  TEST_ASSERT_TRUE(response.view().find("MAINTENANCE_REQUIRED") !=
                   std::string_view::npos);
  TEST_ASSERT_EQUAL_UINT(0U, handoff.calls);

  MaintenanceCommand enter{};
  enter.kind = CommandKind::MaintenanceEnter;
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(enter, 0U, response)));
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(command, 0U, response)));
  TEST_ASSERT_EQUAL_STRING("ble_passkey=654321\n", response.c_str());
  TEST_ASSERT_TRUE(response.view().find("[REDACTED]") == std::string_view::npos);
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Rejected),
                        static_cast<int>(router.execute(command, 0U, response)));
  TEST_ASSERT_TRUE(response.view().find("ALREADY_DISCLOSED") !=
                   std::string_view::npos);
  TEST_ASSERT_EQUAL_UINT(2U, handoff.calls);
}

void test_ble_passkey_parser_and_unavailable_path_do_not_leak() {
  FakeTransport transport{};
  transport.input = "ble passkey\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);
  drain(service, transport);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::BlePasskey),
                        static_cast<int>(sink.last.kind));

  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  FakePasskeyHandoff handoff{};
  handoff.next = sigap::provisioning::PasskeyDisclosureStatus::Unavailable;
  CommandRouter router(config_manager, secrets, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr, &handoff);
  MaintenanceCommand enter{};
  enter.kind = CommandKind::MaintenanceEnter;
  MaintenanceResponse response{};
  (void)router.execute(enter, 0U, response);
  MaintenanceCommand command{};
  command.kind = CommandKind::BlePasskey;
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Rejected),
                        static_cast<int>(router.execute(command, 0U, response)));
  TEST_ASSERT_TRUE(response.view().find("UNAVAILABLE") != std::string_view::npos);
  TEST_ASSERT_TRUE(response.view().find("654321") == std::string_view::npos);
}

void test_ble_passkey_authorization_survives_config_required_refresh() {
  sigap::config::MemoryConfigStore config_store{};
  config_store.set_provisioned(true);
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::ConfigRequired),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  FakePasskeyHandoff handoff{};
  CommandRouter router(config_manager, secrets, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr, &handoff);

  MaintenanceCommand enter{};
  enter.kind = CommandKind::MaintenanceEnter;
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(enter, 0U, response)));
  MaintenanceCommand command{};
  command.kind = CommandKind::BlePasskey;
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(command, 1U, response)));
  TEST_ASSERT_EQUAL_STRING("ble_passkey=654321\n", response.c_str());
}

void test_service_polls_without_input_and_reports_disconnect_once() {
  FakeTransport transport{};
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  service.poll(10U);
  TEST_ASSERT_TRUE(sink.poll_calls >= 1U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.transport_lost_calls);

  transport.link = false;
  service.poll(20U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.transport_lost_calls);
  service.poll(30U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.transport_lost_calls);
}

void test_disconnect_discards_partial_parser_line() {
  FakeTransport transport{};
  transport.input = "status";
  FakeSink sink{};
  MaintenanceService service(transport, sink);
  service.poll(10U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.calls);

  transport.link = false;
  service.poll(20U);
  transport.link = true;
  transport.input += "\n";
  drain(service, transport, 30U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.calls);
}

void test_profile_import_round_trip_uses_real_router_and_explicit_activation() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  MemoryProfileStore profile_store{};
  ProfilePersistence persistence(profile_store);
  ProfileCodec codec{};
  EncodedProfile v1{};
  EncodedProfile v2{};
  TEST_ASSERT_TRUE(codec.encode(make_profile(1U, 500.0F), v1));
  TEST_ASSERT_TRUE(codec.encode(make_profile(2U, 600.0F), v2));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(
                            ProfileTarget::Hx711Organic, v1)));
  ProfileManager manager(persistence, codec);
  TEST_ASSERT_TRUE(manager.load(ProfileTarget::Hx711Organic));

  CommandRouter router(config_manager, secrets, &manager);
  FakeTransport transport{};
  MaintenanceService service(transport, router);
  const std::string encoded = hex(v2);
  const std::string digest = hash_hex(v2);
  transport.input = "profile import begin hx711_organic " +
                    std::to_string(v2.size) + " hash=" + digest + "\n";
  for (std::size_t offset = 0U; offset < encoded.size(); offset += 48U) {
    const std::size_t count = std::min<std::size_t>(48U, encoded.size() - offset);
    transport.input += "profile import chunk " +
                       std::to_string(offset / 2U) + " " +
                       encoded.substr(offset, count) + "\n";
  }
  transport.input += "profile import finish hx711_organic\n";
  drain(service, transport);

  ActiveProfileSnapshot snapshot{};
  TEST_ASSERT_TRUE(manager.snapshot(ProfileTarget::Hx711Organic, snapshot));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(v1.hash().data(), snapshot.ref.hash.data(),
                                v1.hash().size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileManagerState::Draft),
                        static_cast<int>(manager.state(ProfileTarget::Hx711Organic)));

  transport.offset = 0U;
  transport.input = "profile validate hx711_organic\n"
                    "profile preview hx711_organic\n"
                    "profile commit hx711_organic\n";
  drain(service, transport);
  TEST_ASSERT_TRUE(manager.snapshot(ProfileTarget::Hx711Organic, snapshot));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(v2.hash().data(), snapshot.ref.hash.data(),
                                v2.hash().size());

  ProfileManager reloaded(persistence, codec);
  TEST_ASSERT_TRUE(reloaded.load(ProfileTarget::Hx711Organic));
  ActiveProfileSnapshot persisted{};
  TEST_ASSERT_TRUE(reloaded.snapshot(ProfileTarget::Hx711Organic, persisted));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(v2.hash().data(), persisted.ref.hash.data(),
                                v2.hash().size());
}

void test_profile_import_round_trip_covers_both_sht31_addresses() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  MemoryProfileStore profile_store{};
  ProfilePersistence persistence(profile_store);
  ProfileCodec codec{};
  ProfileManager manager(persistence, codec);
  CommandRouter router(config_manager, secrets, &manager);
  FakeTransport transport{};
  MaintenanceService service(transport, router);

  struct Fixture {
    const char* key;
    ProfileTarget target;
    uint8_t address;
  };
  constexpr Fixture fixtures[] = {
      {"sht31_44", ProfileTarget::Sht31_44, 0x44U},
      {"sht31_45", ProfileTarget::Sht31_45, 0x45U},
  };

  for (const auto& fixture : fixtures) {
    Sht31Profile v1_profile =
        make_sht_profile(1U, fixture.address, fixture.target);
    Sht31Profile v2_profile =
        make_sht_profile(2U, fixture.address, fixture.target);
    v2_profile.temperature_offset_centi_c = 40;
    EncodedProfile v1{};
    EncodedProfile v2{};
    TEST_ASSERT_TRUE(codec.encode(v1_profile, v1));
    TEST_ASSERT_TRUE(codec.encode(v2_profile, v2));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProfileCommitResult::Committed),
        static_cast<int>(persistence.commit(fixture.target, v1)));
    TEST_ASSERT_TRUE(manager.load(fixture.target));

    const std::string encoded = hex(v2);
    transport.output.clear();
    transport.offset = 0U;
    transport.input = "profile import begin " + std::string(fixture.key) +
                      " " + std::to_string(v2.size) + " hash=" +
                      hash_hex(v2) + "\n";
    for (std::size_t offset = 0U; offset < encoded.size(); offset += 48U) {
      const std::size_t count =
          std::min<std::size_t>(48U, encoded.size() - offset);
      transport.input += "profile import chunk " +
                         std::to_string(offset / 2U) + " " +
                         encoded.substr(offset, count) + "\n";
    }
    transport.input += "profile import finish " + std::string(fixture.key) +
                       "\n";
    drain(service, transport);

    ActiveProfileSnapshot snapshot{};
    TEST_ASSERT_TRUE(manager.snapshot(fixture.target, snapshot));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(v1.hash().data(), snapshot.ref.hash.data(),
                                  v1.hash().size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileManagerState::Draft),
                          static_cast<int>(manager.state(fixture.target)));

    transport.output.clear();
    transport.offset = 0U;
    transport.input = "profile validate " + std::string(fixture.key) +
                      "\nprofile preview " + std::string(fixture.key) +
                      "\nprofile commit " + std::string(fixture.key) + "\n";
    drain(service, transport);
    TEST_ASSERT_TRUE(manager.snapshot(fixture.target, snapshot));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(v2.hash().data(), snapshot.ref.hash.data(),
                                  v2.hash().size());

    ProfileManager reloaded(persistence, codec);
    TEST_ASSERT_TRUE(reloaded.load(fixture.target));
    ActiveProfileSnapshot persisted{};
    TEST_ASSERT_TRUE(reloaded.snapshot(fixture.target, persisted));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(v2.hash().data(), persisted.ref.hash.data(),
                                  v2.hash().size());
  }
}

void test_profile_import_rejects_unbound_or_out_of_order_chunks() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  MemoryProfileStore profile_store{};
  ProfilePersistence persistence(profile_store);
  ProfileCodec codec{};
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(make_profile(1U, 500.0F), encoded));
  ProfileManager manager(persistence, codec);
  CommandRouter router(config_manager, secrets, &manager);
  FakeTransport transport{};
  MaintenanceService service(transport, router);

  transport.input = "profile import chunk 0 aa\n";
  drain(service, transport);
  TEST_ASSERT_TRUE(transport.output.find("ERR REJECTED") != std::string::npos);

  transport.output.clear();
  const std::string bytes = hex(encoded);
  transport.offset = 0U;
  transport.input = "profile import begin hx711_organic " +
                    std::to_string(encoded.size) + " hash=" + hash_hex(encoded) + "\n";
  drain(service, transport);
  transport.offset = 0U;
  transport.input = "profile import chunk 1 " + bytes.substr(0U, 16U) + "\n";
  drain(service, transport);
  TEST_ASSERT_TRUE(transport.output.find("ERR INVALID_SYNTAX") != std::string::npos);
}

void test_profile_import_timeout_and_disconnect_discard_draft() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  MemoryProfileStore profile_store{};
  ProfilePersistence persistence(profile_store);
  ProfileCodec codec{};
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(make_profile(1U, 500.0F), encoded));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(
                            ProfileTarget::Hx711Organic, encoded)));
  ProfileManager manager(persistence, codec);
  TEST_ASSERT_TRUE(manager.load(ProfileTarget::Hx711Organic));
  CommandRouter router(config_manager, secrets, &manager);

  MaintenanceCommand begin{};
  begin.kind = CommandKind::ProfileImportBegin;
  TEST_ASSERT_TRUE(begin.target.assign("hx711_organic"));
  begin.has_numeric = true;
  begin.numeric_value = static_cast<int64_t>(encoded.size);
  TEST_ASSERT_TRUE(begin.value.assign("hash=" + hash_hex(encoded)));
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(begin, 100U, response)));
  router.poll(100U + CommandRouter::kTransferTimeoutUs + 1U);

  MaintenanceCommand chunk{};
  chunk.kind = CommandKind::ProfileImportChunk;
  chunk.has_numeric = true;
  chunk.numeric_value = 0;
  TEST_ASSERT_TRUE(chunk.value.assign(hex(encoded).substr(0U, 16U)));
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Rejected),
                        static_cast<int>(router.execute(chunk, 500U, response)));

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(begin, 1000U, response)));
  const std::string bytes = hex(encoded);
  for (std::size_t offset = 0U; offset < bytes.size(); offset += 48U) {
    const std::size_t count = std::min<std::size_t>(48U, bytes.size() - offset);
    MaintenanceCommand part{};
    part.kind = CommandKind::ProfileImportChunk;
    part.has_numeric = true;
    part.numeric_value = static_cast<int64_t>(offset / 2U);
    TEST_ASSERT_TRUE(part.value.assign(bytes.substr(offset, count)));
    response = MaintenanceResponse{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                          static_cast<int>(router.execute(part, 1001U, response)));
  }
  MaintenanceCommand finish{};
  finish.kind = CommandKind::ProfileImportFinish;
  TEST_ASSERT_TRUE(finish.target.assign("hx711_organic"));
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(finish, 1002U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileManagerState::Draft),
                        static_cast<int>(manager.state(ProfileTarget::Hx711Organic)));
  router.transport_lost();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileManagerState::Active),
                        static_cast<int>(manager.state(ProfileTarget::Hx711Organic)));
}

void test_calibration_start_binds_target_timeout_and_cancel_before_criteria() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  CommandRouter router(config_manager, secrets);
  MaintenanceCommand start{};
  start.kind = CommandKind::CalibrationStart;
  TEST_ASSERT_TRUE(start.target.assign("ds18b20_b3"));
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(start, 100U, response)));
  TEST_ASSERT_TRUE(router.is_calibrating(sigap::acquisition::SensorId::Ds18b20B3));
  TEST_ASSERT_FALSE(router.is_calibrating(sigap::acquisition::SensorId::Hx711Organic));

  MaintenanceCommand repeated = start;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Rejected),
                        static_cast<int>(router.execute(repeated, 200U, response)));
  router.poll(100U + sigap::calibration::CalibrationSession::kSessionTimeoutUs + 1U);
  TEST_ASSERT_FALSE(router.is_calibrating(sigap::acquisition::SensorId::Ds18b20B3));

  CommandRouter second(config_manager, secrets);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(second.execute(start, 0U, response)));
  MaintenanceCommand cancel{};
  cancel.kind = CommandKind::CalibrationCancel;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(second.execute(cancel, 1U, response)));
  TEST_ASSERT_FALSE(second.is_calibrating(sigap::acquisition::SensorId::Ds18b20B3));
}

void test_empty_store_hx711_calibration_commits_through_parser() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  MemoryProfileStore profile_store{};
  ProfilePersistence persistence(profile_store);
  ProfileCodec codec{};
  ProfileManager manager(persistence, codec);
  TEST_ASSERT_TRUE(manager.load(ProfileTarget::Hx711Organic));

  CalibrationEvidenceFake evidence{};
  evidence.trial = sigap::calibration::Hx711Trial{1000,
      SampleQuality::Uncalibrated, true, true};
  evidence.sequence = 1U;
  CommandRouter router(config_manager, secrets, &manager, &evidence);
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration start hx711_organic\n", 10U,
                            response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration criteria samples 1\n", 11U,
                            response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration criteria max_span_counts 10\n",
                            12U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration criteria reference_mass_mg 1000000\n",
                            13U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration capture tare\n", 14U,
                            response)));

  evidence.trial = sigap::calibration::Hx711Trial{2000,
      SampleQuality::Uncalibrated, true, true};
  evidence.sequence = 2U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration capture reference\n", 15U,
                            response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration validate\n", 16U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration preview\n", 17U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration commit\n", 18U, response)));

  ActiveProfileSnapshot active{};
  TEST_ASSERT_TRUE(manager.snapshot(ProfileTarget::Hx711Organic, active));
  TEST_ASSERT_TRUE(active.present);
  Hx711Profile profile{};
  TEST_ASSERT_TRUE(codec.decode(active.encoded, profile));
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 1000.0F, profile.counts_per_kg);
  ProfileManager reloaded(persistence, codec);
  TEST_ASSERT_TRUE(reloaded.load(ProfileTarget::Hx711Organic));
  ActiveProfileSnapshot persisted{};
  TEST_ASSERT_TRUE(reloaded.snapshot(ProfileTarget::Hx711Organic, persisted));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(active.ref.hash.data(), persisted.ref.hash.data(),
                                active.ref.hash.size());
}

void test_first_time_engineering_criteria_are_bounded_and_typed() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  CalibrationEvidenceFake evidence{};
  CommandRouter router(config_manager, secrets, nullptr, &evidence);
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration start ultrasonic_b3\n", 1U,
                            response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::InvalidNumeric),
                        static_cast<int>(run_router_line(
                            router, "calibration criteria echo_mm_per_us_milli 0\n",
                            2U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::InvalidNumeric),
                        static_cast<int>(run_router_line(
                            router, "calibration criteria echo_mm_per_us_milli 1001\n",
                            3U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration criteria echo_mm_per_us_milli 171\n",
                            4U, response)));
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 0.171F, evidence.ultrasonic_conversion);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router,
                            "calibration criteria overshoot_tolerance_mm_milli 0\n",
                            4U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::InvalidNumeric),
                        static_cast<int>(run_router_line(
                            router, "calibration criteria resolution_bits 13\n",
                            5U, response)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "calibration cancel\n", 6U, response)));
}

void test_fragmented_input_is_buffered_until_line_end() {
  FakeTransport transport{};
  transport.input = "status";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  TEST_ASSERT_EQUAL_UINT32(6U, service.poll(10U).bytes_read);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.calls);

  transport.input += "\n";
  const auto stats = service.poll(20U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Status),
                        static_cast<int>(sink.last.kind));
  TEST_ASSERT_EQUAL_UINT32(1U, stats.commands_dispatched);
  TEST_ASSERT_TRUE(transport.output.find("OK status") != std::string::npos);
}

void test_multiple_commands_are_processed_with_poll_budget() {
  FakeTransport transport{};
  transport.input = "status\nmaintenance enter\nstatus\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  const auto first = service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(kMaxBytesPerPoll, first.bytes_read);
  TEST_ASSERT_TRUE(sink.calls >= 1U);

  while (transport.available()) service.poll(1U);
  TEST_ASSERT_EQUAL_UINT32(3U, sink.calls);
  TEST_ASSERT_EQUAL_UINT32(3U, service.commands_dispatched());
}

void test_oversized_command_is_rejected_and_parser_recovers() {
  FakeTransport transport{};
  transport.input = std::string(kMaxCommandLineBytes + 8U, 'x') +
                    "\nstatus\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  while (transport.available()) service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_TRUE(transport.output.find("ERR OVERSIZED") != std::string::npos);
  TEST_ASSERT_TRUE(transport.output.find("OK status") != std::string::npos);
}

void test_unknown_empty_and_invalid_numeric_commands_are_explicit() {
  FakeTransport transport{};
  transport.input = "\nnot-a-command\ncalibration capture organic nope\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  while (transport.available()) service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.calls);
  TEST_ASSERT_TRUE(transport.output.find("ERR EMPTY") != std::string::npos);
  TEST_ASSERT_TRUE(transport.output.find("ERR UNKNOWN_COMMAND") !=
                  std::string::npos);
  TEST_ASSERT_TRUE(transport.output.find("ERR INVALID_NUMERIC") !=
                  std::string::npos);
}

void test_config_numeric_and_calibration_arguments_are_typed() {
  FakeTransport transport{};
  transport.input = "config set sampling.mq_period_ms 100\n"
                    "calibration capture organic -42\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);
  while (transport.available()) service.poll(0U);

  TEST_ASSERT_EQUAL_UINT32(2U, sink.calls);
  TEST_ASSERT_TRUE(sink.last.has_numeric);
  TEST_ASSERT_EQUAL_INT64(-42, sink.last.numeric_value);
  TEST_ASSERT_EQUAL_STRING("organic", sink.last.target.c_str());
}

void test_response_fits_complete_sd_probe_with_uint64_capacity() {
  std::array<uint8_t, 32U> hash{};
  hash.fill(0xFFU);
  const auto largest = std::numeric_limits<uint64_t>::max();
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
      static_cast<int>(append_sd_probe_pass(
          response, "mmc1", 256U, hash, largest, largest, largest, largest)));
  const std::string maximum = "18446744073709551615";
  const std::string expected =
      "sd=PASS\nbus=mmc1\nn=256\nhash=" + std::string(64U, 'f') +
      "\nus=" + maximum + "\nt=" + maximum + "\nu=" + maximum +
      "\nf=" + maximum + "\nOK\n";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), response.c_str());
  TEST_ASSERT_TRUE(response.size() <= kMaxResponseBytes);
}

void test_response_redacts_secret_and_stays_bounded() {
  MaintenanceResponse response;
  TEST_ASSERT_TRUE(response.append_secret("token", "super-secret"));
  TEST_ASSERT_TRUE(response.append_presence("wifi", true));
  TEST_ASSERT_TRUE(response.size() < kMaxResponseBytes);
  TEST_ASSERT_NOT_EQUAL(nullptr, response.c_str());
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        std::string(response.view()).find("[REDACTED]"));
  TEST_ASSERT_EQUAL(std::string::npos,
                    std::string(response.view()).find("super-secret"));
}

void test_parser_failure_does_not_stop_later_commands() {
  FakeTransport transport{};
  transport.input = "config set sampling.mq_period_ms bad\nstatus\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);
  while (transport.available()) service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Status),
                        static_cast<int>(sink.last.kind));
}

void test_command_router_keeps_draft_isolated_and_redacts_status() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  CommandRouter router(config_manager, secrets);
  MaintenanceCommand start{};
  start.kind = CommandKind::ConfigDraftStart;
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(start, 0U, response)));
  MaintenanceCommand set{};
  set.kind = CommandKind::ConfigSet;
  TEST_ASSERT_TRUE(set.field.assign("sampling.mq_period_ms"));
  set.has_numeric = true;
  set.numeric_value = 250;
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(set, 0U, response)));
  TEST_ASSERT_EQUAL_UINT32(100U,
                           config_manager.active().sampling.mq_period_ms);
  TEST_ASSERT_EQUAL_UINT32(250U,
                           config_manager.draft().sampling.mq_period_ms);
  MaintenanceCommand status{};
  status.kind = CommandKind::Status;
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(status, 0U, response)));
  TEST_ASSERT_TRUE(response.view().find("config_hash=") != std::string_view::npos);
}

void test_sensor_status_parser_requires_one_known_target() {
  MaintenanceParser parser{};
  ParseEvent event{};
  bool emitted{false};
  for (const char character : std::string("sensor status mq7\n")) {
    emitted = parser.consume(character, event) || emitted;
  }
  TEST_ASSERT_TRUE(emitted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(event.status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::SensorStatus),
                        static_cast<int>(event.command.kind));
  TEST_ASSERT_EQUAL_STRING("mq7", event.command.target.c_str());
}

void test_sensor_status_reports_missing_evidence_without_mutation() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
                        static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  sigap::calibration::CalibrationEvidenceCache evidence{};
  CommandRouter router(config_manager, secrets, nullptr, &evidence);
  MaintenanceResponse response{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "sensor status mq7\n", 100U, response)));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("s=mq7\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("av=0\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("q=UNAVAILABLE\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("c=IDLE\n"));
  TEST_ASSERT_TRUE(response.size() <= kMaxResponseBytes);
}

void test_sensor_status_reports_uncalibrated_evidence_and_missing_profile() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
                        static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  MemoryProfileStore profile_store{};
  ProfilePersistence persistence(profile_store);
  ProfileManager profiles(persistence);
  TEST_ASSERT_TRUE(profiles.load(ProfileTarget::Mq4));
  sigap::calibration::CalibrationEvidenceCache evidence{};
  auto sample = diagnostic_sample(SensorId::Mq4, StreamId::Organic,
                                  SampleQuality::Uncalibrated,
                                  SampleReason::ProfileMissing, 1U, 100U);
  sample.raw.present = true;
  sample.raw.identity_valid = true;
  evidence.observe(sample);
  CommandRouter router(config_manager, secrets, &profiles, &evidence);
  MaintenanceResponse response{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "sensor status mq4\n", 101U, response)));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("q=UNCALIBRATED\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("r=PROFILE_MISSING\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("p=0\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("a=1\n"));
}

void test_sensor_status_reports_crc_failure_and_stale_evidence() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
                        static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  sigap::calibration::CalibrationEvidenceCache evidence{};
  auto sample = diagnostic_sample(SensorId::Ds18b20B3, StreamId::B3,
                                  SampleQuality::CrcError,
                                  SampleReason::CrcError, 1U, 100U);
  sample.raw.present = true;
  sample.raw.identity_valid = true;
  sample.raw.crc_valid = false;
  sample.raw.conversion_complete = true;
  sample.raw.ds_wire_evidence = true;
  evidence.observe(sample);
  CommandRouter router(config_manager, secrets, nullptr, &evidence);
  MaintenanceResponse response{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "sensor status ds18b20_b3\n", 5000101U,
                            response)));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("q=CRC_ERROR\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("fr=STALE\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("f=id=1,crc=0,conv=1,wire=1\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("x=1\n"));
  TEST_ASSERT_TRUE(response.size() <= kMaxResponseBytes);
}

void test_sensor_status_reports_active_profile_and_isolates_channels() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
                        static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  MemoryProfileStore profile_store{};
  ProfilePersistence persistence(profile_store);
  ProfileCodec codec{};
  EncodedProfile encoded{};
  TEST_ASSERT_TRUE(codec.encode(make_profile(7U, 1000.0F), encoded));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileCommitResult::Committed),
                        static_cast<int>(persistence.commit(ProfileTarget::Hx711Organic,
                                                            encoded)));
  ProfileManager profiles(persistence, codec);
  TEST_ASSERT_TRUE(profiles.load(ProfileTarget::Hx711Organic));
  sigap::calibration::CalibrationEvidenceCache evidence{};
  auto hx = diagnostic_sample(SensorId::Hx711Organic, StreamId::Organic,
                              SampleQuality::Ready, SampleReason::DataReady,
                              3U, 1000U);
  hx.raw.present = true;
  hx.raw.identity_valid = true;
  evidence.observe(hx);
  auto other = diagnostic_sample(SensorId::Mq7, StreamId::B3,
                                 SampleQuality::Timeout, SampleReason::Timeout,
                                 4U, 1001U);
  evidence.observe(other);
  CommandRouter router(config_manager, secrets, &profiles, &evidence);
  MaintenanceResponse response{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "sensor status hx711_organic\n", 1002U,
                            response)));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("st=organic\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("q=READY\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("p=1\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("pv=7\n"));
  TEST_ASSERT_NOT_EQUAL(std::string_view::npos, response.view().find("a=1\n"));
  TEST_ASSERT_EQUAL(std::string_view::npos, response.view().find("mq7"));
}

void test_sensor_status_is_bounded_and_redacts_sensitive_or_raw_values() {
  using namespace sigap::acquisition;
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
                        static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  TEST_ASSERT_TRUE(secrets.rotate_device_token("diagnostic-secret"));
  sigap::calibration::CalibrationEvidenceCache evidence{};
  auto sample = diagnostic_sample(SensorId::Sht31_45, StreamId::B3,
                                  SampleQuality::Ready, SampleReason::DataReady,
                                  1U, 100U);
  sample.raw.present = true;
  sample.raw.identity_valid = true;
  sample.raw.temperature_crc_valid = true;
  sample.raw.humidity_crc_valid = true;
  sample.raw.temperature_centi_c = 12345;
  sample.raw.humidity_centi_pct = 54321;
  evidence.observe(sample);
  CommandRouter router(config_manager, secrets, nullptr, &evidence);
  MaintenanceResponse response{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(run_router_line(
                            router, "sensor status sht31_45\n", 101U,
                            response)));
  TEST_ASSERT_TRUE(response.size() <= kMaxResponseBytes);
  TEST_ASSERT_EQUAL(std::string_view::npos,
                    response.view().find("diagnostic-secret"));
  TEST_ASSERT_EQUAL(std::string_view::npos, response.view().find("12345"));
  TEST_ASSERT_EQUAL(std::string_view::npos, response.view().find("54321"));
}

void test_maintenance_memory_footprint_is_bounded() {
  std::printf("memory MaintenanceParser=%zu MaintenanceResponse=%zu "
              "CommandRouter=%zu MaintenanceService=%zu\n",
              sizeof(MaintenanceParser), sizeof(MaintenanceResponse),
              sizeof(CommandRouter), sizeof(MaintenanceService));
  TEST_ASSERT_TRUE(sizeof(MaintenanceResponse) <= 256U);
  TEST_ASSERT_TRUE(sizeof(MaintenanceService) <= 512U);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_fragmented_input_is_buffered_until_line_end);
  RUN_TEST(test_profile_lifecycle_commands_parse_as_explicit_steps);
  RUN_TEST(test_storage_sd_probe_command_is_bounded_and_explicit);
  RUN_TEST(test_ble_passkey_command_requires_maintenance_and_discloses_once);
  RUN_TEST(test_ble_passkey_parser_and_unavailable_path_do_not_leak);
  RUN_TEST(test_ble_passkey_authorization_survives_config_required_refresh);
  RUN_TEST(test_service_polls_without_input_and_reports_disconnect_once);
  RUN_TEST(test_disconnect_discards_partial_parser_line);
  RUN_TEST(test_profile_import_round_trip_uses_real_router_and_explicit_activation);
  RUN_TEST(test_profile_import_round_trip_covers_both_sht31_addresses);
  RUN_TEST(test_profile_import_rejects_unbound_or_out_of_order_chunks);
  RUN_TEST(test_profile_import_timeout_and_disconnect_discard_draft);
  RUN_TEST(test_calibration_start_binds_target_timeout_and_cancel_before_criteria);
  RUN_TEST(test_empty_store_hx711_calibration_commits_through_parser);
  RUN_TEST(test_first_time_engineering_criteria_are_bounded_and_typed);
  RUN_TEST(test_multiple_commands_are_processed_with_poll_budget);
  RUN_TEST(test_oversized_command_is_rejected_and_parser_recovers);
  RUN_TEST(test_unknown_empty_and_invalid_numeric_commands_are_explicit);
  RUN_TEST(test_config_numeric_and_calibration_arguments_are_typed);
  RUN_TEST(test_response_fits_complete_sd_probe_with_uint64_capacity);
  RUN_TEST(test_response_redacts_secret_and_stays_bounded);
  RUN_TEST(test_parser_failure_does_not_stop_later_commands);
  RUN_TEST(test_command_router_keeps_draft_isolated_and_redacts_status);
  RUN_TEST(test_sensor_status_parser_requires_one_known_target);
  RUN_TEST(test_sensor_status_reports_missing_evidence_without_mutation);
  RUN_TEST(test_sensor_status_reports_uncalibrated_evidence_and_missing_profile);
  RUN_TEST(test_sensor_status_reports_crc_failure_and_stale_evidence);
  RUN_TEST(test_sensor_status_reports_active_profile_and_isolates_channels);
  RUN_TEST(test_sensor_status_is_bounded_and_redacts_sensitive_or_raw_values);
  RUN_TEST(test_maintenance_memory_footprint_is_bounded);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
