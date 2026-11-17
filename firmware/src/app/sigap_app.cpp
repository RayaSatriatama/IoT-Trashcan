#include "maintenance/storage_probe_response.h"
#include "sigap_app.h"
#if SIGAP_SENSOR_SIMULATION
#include "acquisition/simulation_profiles.h"
#endif

#include <array>
#include <cstdio>
#include <ctime>
#include <string_view>

#include "acquisition/default_profiles.h"
#include "acquisition/profiles.h"
#include "sigap/board/board_memory.h"
#include "sigap/board/sd_transport.h"
#include "sigap/build/build_config.h"
#include "acquisition/provenance.h"
#include "acquisition/arduino_adapters.h"
#include "acquisition/usb_sensor_debug.h"
#include "storage/sd_probe_path.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_littlefs.h>
#include <esp_system.h>
#include <esp_timer.h>
#ifdef SIGAP_USE_SD
#ifdef SIGAP_SD_TRANSPORT_SDMMC_1BIT
#include <SD_MMC.h>
#else
#include <SD.h>
#include <SPI.h>
#endif
#endif
#endif
#ifdef ARDUINO
#include "telemetry/sha256.h"
#endif

namespace sigap {

namespace {
acquisition::EncodedProfile sht31_factory_profile() noexcept {
  acquisition::Sht31Profile profile{};
  profile.manifest.kind = acquisition::ProfileKind::Calibration;
  profile.manifest.schema_version = acquisition::kProfileSchemaVersion;
  profile.manifest.profile_version = 1U;
  profile.manifest.target_id = static_cast<uint32_t>(acquisition::ProfileTarget::Sht31_44);
  profile.manifest.validity = acquisition::ProfileValidity::Valid;
  profile.manifest.provenance.present = true;
  profile.manifest.provenance.reference_note = {'F','A','C','C','T','O','R','Y','_','D','E','F','A','U','L','T'};
  profile.address = 0x44U;
  profile.temperature_offset_centi_c = 0;
  profile.humidity_offset_centi_pct = 0;
  acquisition::EncodedProfile encoded{};
  (void)acquisition::ProfilePersistence::default_codec().encode(profile, encoded);
  return encoded;
}
}

#ifdef ARDUINO
std::size_t ApplicationDebugUsbWriter::writable() noexcept {
  const int available = Serial.availableForWrite();
  return available < 384 ? 0U : static_cast<std::size_t>(available);
}

std::size_t ApplicationDebugUsbWriter::write(const char* bytes,
                                              std::size_t size) noexcept {
  return Serial.write(reinterpret_cast<const uint8_t*>(bytes), size);
}
#endif

#ifdef ARDUINO
namespace {
uint8_t reset_reason_code() noexcept {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return 1U;
    case ESP_RST_BROWNOUT: return 2U;
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_INT_WDT: return 3U;
    case ESP_RST_SW: return 4U;
    default: return 0U;
  }
}

#ifdef SIGAP_USE_SD
bool sd_transport_pins_configured{false};
bool sd_transport_mounted{false};

bool begin_sd_transport() noexcept {
  const auto& policy = board::kSdTransportPolicy;
#ifdef SIGAP_SD_TRANSPORT_SDMMC_1BIT
  // Arduino-ESP32 requires setPins() before the first SD_MMC.begin() only.
  // The maintenance probe may run after setup has mounted the same singleton;
  // repeating setPins() then fails and falsely reports a media outage.
  if (!sd_transport_pins_configured) {
    SD_MMC.setPins(policy.clk, policy.cmd_or_miso, policy.d0_or_mosi);
    sd_transport_pins_configured = true;
  }
  if (sd_transport_mounted) return true;
  sd_transport_mounted = SD_MMC.begin(
      policy.mount_point, policy.mode_1bit, policy.format_if_mount_failed,
      SDMMC_FREQ_DEFAULT, policy.max_files);
  return sd_transport_mounted;
#else
  if (sd_transport_mounted) return true;
  SPI.begin(policy.clk, policy.cmd_or_miso, policy.d0_or_mosi, policy.cs);
  sd_transport_mounted = SD.begin(policy.cs, SPI, policy.spi_frequency_hz);
  return sd_transport_mounted;
#endif
}

fs::FS& sd_filesystem() noexcept {
#ifdef SIGAP_SD_TRANSPORT_SDMMC_1BIT
  return SD_MMC;
#else
  return SD;
#endif
}

uint64_t sd_total_bytes() noexcept {
#ifdef SIGAP_SD_TRANSPORT_SDMMC_1BIT
  return SD_MMC.totalBytes();
#else
  return SD.totalBytes();
#endif
}

uint64_t sd_used_bytes() noexcept {
#ifdef SIGAP_SD_TRANSPORT_SDMMC_1BIT
  return SD_MMC.usedBytes();
#else
  return SD.usedBytes();
#endif
}
#endif
}  // namespace

maintenance::CommandStatus ArduinoSdProbe::run_sd_probe(
    maintenance::MaintenanceResponse& response) noexcept {
#ifdef SIGAP_USE_SD
  static constexpr std::array<uint8_t, 256U> kProbeBytes = [] {
    std::array<uint8_t, 256U> value{};
    for (std::size_t index = 0U; index < value.size(); ++index) {
      value[index] = static_cast<uint8_t>(index ^ 0x5AU);
    }
    return value;
  }();
  char probe_path[64U]{};
  const uint64_t device_id = ESP.getEfuseMac();
  if (!storage::build_sd_probe_path(device_id, probe_path, sizeof(probe_path)))
    return maintenance::CommandStatus::Failed;
  const int64_t started_us = esp_timer_get_time();
  if (!begin_sd_transport()) {
    return maintenance::CommandStatus::Rejected;
  }
  fs::FS& sd = sd_filesystem();
  bool archive_verified = false;
  if (archive_store_ != nullptr) {
    if (archive_quota_bytes_ == 0U ||
        !archive_store_->begin(sd, archive_quota_bytes_)) {
      return maintenance::CommandStatus::Failed;
    }
    archive_identity_ = storage::ArchiveIdentity{};
    archive_hash_.fill(0U);
    if (archive_store_->append_probe_record(
            static_cast<uint64_t>(started_us < 0 ? 0 : started_us),
            archive_identity_, archive_hash_) !=
        storage::AppendStatus::Durable) {
      return maintenance::CommandStatus::Failed;
    }
    // Re-open and recover the same store to prove the archive frame survived
    // the filesystem boundary, then remove only this probe record.
    archive_verified = archive_store_->begin(sd, archive_quota_bytes_) &&
                       archive_store_->contains(archive_identity_, archive_hash_) &&
                       archive_store_->reclaim_pending(archive_identity_,
                                                       archive_hash_) &&
                       !archive_store_->contains(archive_identity_, archive_hash_);
    if (!archive_verified) {
      return maintenance::CommandStatus::Failed;
    }
  }
  // A pre-existing path may be a leftover from an interrupted run or an
  // unrelated file. Never remove or overwrite it as part of a probe.
  File existing = sd.open(probe_path, FILE_READ);
  if (existing) {
    existing.close();
    return maintenance::CommandStatus::Failed;
  }
  File output = sd.open(probe_path, FILE_WRITE);
  if (!output) {
    return maintenance::CommandStatus::Failed;
  }
  const bool written = output.write(kProbeBytes.data(), kProbeBytes.size()) ==
                       kProbeBytes.size();
  if (written) output.flush();
  const bool flushed = written;
  output.close();
  if (!flushed) {
    (void)sd.remove(probe_path);
    return maintenance::CommandStatus::Failed;
  }
  File input = sd.open(probe_path, FILE_READ);
  if (!input || input.size() != kProbeBytes.size()) {
    if (input) input.close();
    (void)sd.remove(probe_path);
    return maintenance::CommandStatus::Failed;
  }
  input.close();
  const auto expected_hash = telemetry_contract::sha256(std::string_view(
      reinterpret_cast<const char*>(kProbeBytes.data()), kProbeBytes.size()));
  config::ConfigHash actual_hash{};
  const bool hash_match = archive_store_ != nullptr &&
                          archive_store_->verify_probe_file(
                              sd, probe_path, kProbeBytes.data(), kProbeBytes.size(),
                              actual_hash) &&
                          expected_hash == actual_hash;
  const bool removed = sd.remove(probe_path);
  if (!hash_match || !removed) {
    return maintenance::CommandStatus::Failed;
  }
  const uint64_t total_bytes = sd_total_bytes();
  const uint64_t used_bytes = sd_used_bytes();
  const uint64_t free_bytes = total_bytes >= used_bytes
                                  ? total_bytes - used_bytes
                                  : 0U;

  const int64_t ended_us = esp_timer_get_time();
  return maintenance::append_sd_probe_pass(
      response,
      board::kSdTransportPolicy.transport == board::SdTransport::SdMmc1Bit
          ? "mmc1" : "spi",
      static_cast<uint16_t>(kProbeBytes.size()), actual_hash,
      ended_us >= started_us ? static_cast<uint64_t>(ended_us - started_us) : 0U,
      total_bytes, used_bytes, free_bytes, archive_verified);
#else
  (void)response;
  return maintenance::CommandStatus::Rejected;
#endif
}
#endif

SigapApp::SigapApp() noexcept
    : config_manager_(config_store_,
                      config::ValidationContext{
                      build::kPreMountLittlefsValidationBytes, 0}),
      record_context_{boot_identity_.boot_id, acquisition::DeviceId{},
                      acquisition::ConfigRef{}, nullptr},
      record_factory_(boot_identity_.sequence, record_context_),
      profile_persistence_(profile_store_),
      profile_manager_(profile_persistence_,
                       acquisition::ProfilePersistence::default_codec(),
                       &audit_ring_),
      acquisition_(config_manager_, record_factory_, board::kN16R8Pins,
                   profiles_),
      archive_store_(sd_store_, littlefs_store_),
      migration_source_(littlefs_store_, storage::ArchiveMedia::LittleFs),
      migration_destination_(&sd_store_, storage::ArchiveMedia::Sd),
      media_migration_(migration_source_, migration_destination_,
                       storage::ArchiveMedia::LittleFs,
                       storage::ArchiveMedia::Sd,
                       storage::MigrationBudget{},
#ifdef ARDUINO
                       &migration_journal_),
#else
                       nullptr),
#endif
      model_lifecycle_(model_persistence_,
                       models::ModelProfilePersistence::default_codec()),
      model_router_(model_lifecycle_,
                    models::ModelProfilePersistence::default_codec()),
      maintenance_router_(config_manager_, secret_store_, &profile_manager_,
                          &evidence_cache_, &reboot_controller_, &audit_ring_,
                          &boot_identity_.boot_id, &model_router_,
#ifdef ARDUINO
                          &sd_probe_),
#else
                          nullptr),
#endif
#ifdef ARDUINO
      calibration_controller_(maintenance_router_, 0U, &calibration_authorizer_,
                              &calibration_replay_store_),
      ble_calibration_backend_(maintenance_router_, provisioning_clock_,
                               &calibration_controller_),
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
      device_control_executor_(maintenance_router_, calibration_controller_),
      device_control_poller_(
          network::NetworkPolicy{1000U, 60000U, 10000U, 60000U, 250U},
          network_clock_, control_transport_boundary_, device_control_executor_,
          &device_control_result_journal_),
#endif
#endif
      network_source_(batch_runtime_),
#ifdef ARDUINO
      wifi_manager_(secret_store_, wifi_station_, provisioning_clock_),
      wifi_backend_(wifi_manager_, io_gate_),
      ble_session_(wifi_backend_, provisioning_clock_),
#endif
      maintenance_service_(maintenance_transport_, maintenance_router_) {
#ifdef ARDUINO
  record_context_.utc_source = &utc_source_;
  record_factory_.set_utc_source(&utc_source_);
#endif
#if defined(ARDUINO) && defined(SIGAP_NETWORK_ENDPOINT) && \
    defined(SIGAP_NETWORK_CA_CERT)
  network_link_.bind(&network_transport_);
#endif
#ifdef ARDUINO
  ble_session_.bind_debug_sink(&debug_service_);
  ble_session_.set_debug_backend(&debug_command_backend_);
  maintenance_router_.set_debug_commands(&debug_command_backend_);
  maintenance_router_.set_calibration_transport_controller(
      &calibration_controller_);
  calibration_controller_.set_debug_sink(&debug_service_);
  maintenance_router_.set_io_gate(&io_gate_);
  model_router_.set_io_gate(&io_gate_);
  ble_calibration_backend_.set_io_gate(&io_gate_);
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  device_control_executor_.set_io_gate(&io_gate_);
#endif
#endif
  sd_probe_.bind_archive_store(
      sd_store_, config_manager_.active().storage.sd_pending_quota_bytes);
  profile_manager_.bind_runtime_profiles(profiles_);
  evidence_cache_.set_profiles(&profiles_);
  archive_runtime_.set_store(storage::live_delivery::kPriorityFresh
                                 ? static_cast<storage::IArchiveStore*>(
                                       &littlefs_store_)
                                 : static_cast<storage::IArchiveStore*>(
                                       &archive_store_));
  archive_runtime_.set_media_migration(
      storage::live_delivery::kPriorityFresh ? nullptr : &media_migration_);
  archive_runtime_.set_sequence_allocator(&boot_identity_.sequence);
  batch_runtime_.bind(storage::live_delivery::kPriorityFresh
                          ? static_cast<storage::IArchiveStore*>(
                                &littlefs_store_)
                          : static_cast<storage::IArchiveStore*>(
                                &archive_store_),
                      &batch_store_, &ack_journal_);
  archive_runtime_.set_event_context(record_context_.device_id,
                                     record_context_.boot_id,
                                     record_context_.config_ref);
#ifdef ARDUINO
  processing_runtime_.set_downstream_observer(&runtime_debug_observer_);
#else
  processing_runtime_.set_downstream_observer(&evidence_cache_);
#endif
  acquisition_.set_sample_observer(&processing_runtime_);
  acquisition_.set_calibration_state_provider(&maintenance_router_);
}

void SigapApp::refresh_model_profiles() noexcept {
  const std::array<models::ModelProfileKey, 3U> fis_keys{{
      {acquisition::ProfileKind::Fis, models::ModelTarget::FisOrganic},
      {acquisition::ProfileKind::Fis, models::ModelTarget::FisInorganic},
      {acquisition::ProfileKind::Fis, models::ModelTarget::FisB3}}};
  const models::ModelProfileKey safety_key{
      acquisition::ProfileKind::Safety, models::ModelTarget::SafetyB3};
  models::ModelProfileSnapshot snapshot{};
  fis_model_present_ = false;
  safety_model_present_ = false;
  std::array<models::FisModelProfile*, 3U> fis_models{{
      &fis_model_, &fis_model_inorganic_, &fis_model_b3_}};
  std::array<const models::FisModelProfile*, 3U> fis_refs{};
#ifdef ARDUINO
  const auto observe_model = [this](std::size_t index,
                                    const models::ModelProfileSnapshot& current,
                                    bool usable) noexcept {
    const uint8_t state = !current.present ? 1U : usable ? 0U : 2U;
    const uint32_t version = current.present ? current.ref.profile_version : 0U;
    if (observed_model_valid_[index] &&
        observed_model_states_[index] == state &&
        observed_model_versions_[index] == version) return;
    observed_model_valid_[index] = true;
    observed_model_states_[index] = state;
    observed_model_versions_[index] = version;
    debug::EventInput event{};
    event.monotonic_us = acquisition::ArduinoMonotonicClock{}.now_us();
    event.module = debug::Module::Calibration;
    event.event_code = 53U;
    event.level = debug::Level::Info;
    event.arg0 = state;
    event.arg1 = version > static_cast<uint32_t>(INT32_MAX)
                     ? INT32_MAX : static_cast<int32_t>(version);
    (void)debug_service_.emit(event);
  };
#endif
  for (std::size_t index = 0U; index < fis_keys.size(); ++index) {
    snapshot = models::ModelProfileSnapshot{};
    const bool found = model_lifecycle_.snapshot(fis_keys[index], snapshot);
    const bool decoded = found && snapshot.present &&
        models::ModelProfilePersistence::default_codec().decode(
            snapshot.encoded, *fis_models[index]);
#ifdef ARDUINO
    if (found) observe_model(index, snapshot, decoded);
#endif
    if (decoded) {
      fis_refs[index] = fis_models[index];
      fis_model_present_ = true;
    }
  }
  snapshot = models::ModelProfileSnapshot{};
  const bool safety_found = model_lifecycle_.snapshot(safety_key, snapshot);
  if (safety_found && snapshot.present) {
    safety_model_present_ = models::ModelProfilePersistence::default_codec().decode(
        snapshot.encoded, safety_model_);
  }
#ifdef ARDUINO
  if (safety_found) observe_model(3U, snapshot, safety_model_present_);
#endif
  processing_runtime_.set_model_profiles(
      fis_refs,
      safety_model_present_ ? &safety_model_ : nullptr);
}

#ifdef ARDUINO
void SigapApp::log_runtime_diagnostic(RuntimeDiagnosticEvent event, bool ready,
                                       uint64_t now_us) noexcept {
  if (!runtime_diagnostics_.first(event)) return;
  const char* name = "UNKNOWN";
  switch (event) {
    case RuntimeDiagnosticEvent::SetupComplete: name = "SETUP_COMPLETE"; break;
    case RuntimeDiagnosticEvent::LoopAlive: name = "LOOP_ALIVE"; break;
    case RuntimeDiagnosticEvent::FilesystemRecovery: name = "FILESYSTEM_RECOVERY"; break;
    case RuntimeDiagnosticEvent::ConfigRuntimeReady: name = "CONFIG_RUNTIME_READY"; break;
    case RuntimeDiagnosticEvent::LittlefsMounted: name = "LITTLEFS_MOUNTED"; break;
    case RuntimeDiagnosticEvent::JournalReady: name = "JOURNAL_READY"; break;
    case RuntimeDiagnosticEvent::NetworkSecretsLoaded: name = "NETWORK_SECRETS_LOADED"; break;
    case RuntimeDiagnosticEvent::NetworkReady: name = "NETWORK_READY"; break;
    case RuntimeDiagnosticEvent::WifiConnected: name = "WIFI_CONNECTED"; break;
    case RuntimeDiagnosticEvent::SntpTimeReady: name = "SNTP_TIME_READY"; break;
    case RuntimeDiagnosticEvent::DeviceControlReady: name = "DEVICE_CONTROL_READY"; break;
    case RuntimeDiagnosticEvent::HttpsPollEntered: name = "HTTPS_POLL_ENTERED"; break;
    case RuntimeDiagnosticEvent::Count: break;
  }
  const uint64_t elapsed_ms = now_us >= runtime_diagnostics_started_us_
                                  ? (now_us - runtime_diagnostics_started_us_) / 1000U
                                  : 0U;
  Serial.printf("[SIGAP][RUNTIME] %s=%s elapsed_ms=%llu\n", name,
                ready ? "PASS" : "FAIL",
                static_cast<unsigned long long>(elapsed_ms));
}
#endif

// cppcheck-suppress unusedFunction
void SigapApp::setup() noexcept {
#ifdef ARDUINO
  previous_loop_entry_us_ = 0U;
  last_loop_gap_class_ = 255U;
  loop_gap_baseline_valid_ = false;
  legacy_retention_reported_ = false;
#endif
  const auto emit_runtime_startup = [this](uint16_t code, int32_t arg0,
                                            int32_t arg1) noexcept {
    debug::EventInput event{};
#ifdef ARDUINO
    event.monotonic_us = acquisition::ArduinoMonotonicClock{}.now_us();
#else
    event.monotonic_us = 0U;
#endif
    event.module = debug::Module::Runtime;
    event.event_code = code;
    event.level = debug::Level::Info;
    event.arg0 = arg0;
    event.arg1 = arg1;
    (void)debug_service_.emit(event);
  };
  emit_runtime_startup(1U, 0, 1);
#ifdef ARDUINO
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
#ifdef SIGAP_USE_SD
  Serial.println("[SIGAP][RUNTIME] SD_BACKUP=ENABLED");
#else
  Serial.println("[SIGAP][RUNTIME] SD_BACKUP=DISABLED");
#endif
#if SIGAP_PROTOTYPE_OPERATION
  Serial.println("[SIGAP][RUNTIME] SIGAP_PROTOTYPE_OPERATION=1");
#else
  Serial.println("[SIGAP][RUNTIME] SIGAP_PROTOTYPE_OPERATION=0");
#endif
  board::log_runtime_board_memory();
  const int64_t started_us = esp_timer_get_time();
  runtime_diagnostics_started_us_ =
      started_us < 0 ? 0U : static_cast<uint64_t>(started_us);
  provisioning_clock_.set_now(
      started_us < 0 ? 0U : static_cast<uint64_t>(started_us));
  const acquisition::InjectedMonotonicClock debug_clock{
      [](void*) noexcept { return acquisition::ArduinoMonotonicClock{}.now_us(); },
      nullptr};
  archive_runtime_.bind_debug(&debug_service_, debug_clock);
  batch_runtime_.bind_debug(&debug_service_, debug_clock);
  acquisition_.bind_debug_sink(&debug_service_);
  processing_runtime_.bind_debug_sink(&debug_service_);
  wifi_manager_.bind_debug_sink(&debug_service_);
  (void)wifi_manager_.begin();
  char ble_name[32]{};
  const uint64_t device_mac = ESP.getEfuseMac();
  acquisition::DeviceId device_identity{};
  device_identity.present = true;
  for (std::size_t index = 0U; index < sizeof(device_mac); ++index) {
    device_identity.bytes[device_identity.bytes.size() - sizeof(device_mac) + index] =
        static_cast<uint8_t>(device_mac >>
                             ((sizeof(device_mac) - 1U - index) * 8U));
  }
  record_context_.device_id = device_identity;
  record_factory_.set_device_id(device_identity);
  ble_session_.set_device_id(device_mac);
  (void)std::snprintf(ble_name, sizeof(ble_name), "SIGAP-%04llX",
                      static_cast<unsigned long long>(device_mac & 0xffffU));
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  // The device-control route keys on the same 32-character lowercase identity
  // that telemetry records carry, not the compact uppercase MAC text.
  std::size_t control_id_size{0U};
  for (std::size_t index = 0U; index < device_identity.bytes.size(); ++index) {
    if (control_id_size + 2U >= device_control_id_.size()) {
      control_id_size = 0U;
      break;
    }
    (void)std::snprintf(device_control_id_.data() + control_id_size,
                        device_control_id_.size() - control_id_size, "%02x",
                        static_cast<unsigned>(device_identity.bytes[index]));
    control_id_size += 2U;
  }
  if (control_id_size != 2U * device_identity.bytes.size()) {
    device_control_id_[0] = '\0';
  }
  calibration_controller_.set_device_id(device_mac);
  calibration_authorizer_.set_device_id(device_mac);
  device_control_transport_.bind_clock(network_clock_);
#endif
  (void)ble_service_.begin(ble_session_, std::string_view(ble_name));
#endif
  emit_runtime_startup(2U, 0, 2);
  const config::ConfigBootResult boot_result = config_manager_.begin();
  (void)boot_result;
#ifdef ARDUINO
  sd_probe_.bind_archive_store(
      sd_store_, config_manager_.active().storage.sd_pending_quota_bytes);
#endif
  for (uint32_t target = 1U; target <= 13U; ++target) {
    (void)profile_manager_.load(
        static_cast<acquisition::ProfileTarget>(target));
  }
  (void)profile_manager_.load(acquisition::ProfileKey{
      acquisition::ProfileKind::Heater, acquisition::ProfileTarget::Mq7});
  (void)profile_manager_.load(acquisition::kMq135GasKey);
  for (uint32_t target = 1U; target <= 3U; ++target) {
    (void)profile_manager_.load(acquisition::ProfileKey{
        acquisition::ProfileKind::Capacity,
        static_cast<acquisition::ProfileTarget>(target)});
  }
  (void)model_lifecycle_.load(models::ModelProfileKey{
      acquisition::ProfileKind::Fis, models::ModelTarget::FisOrganic});
  (void)model_lifecycle_.load(models::ModelProfileKey{
      acquisition::ProfileKind::Fis, models::ModelTarget::FisInorganic});
  (void)model_lifecycle_.load(models::ModelProfileKey{
      acquisition::ProfileKind::Fis, models::ModelTarget::FisB3});
  (void)model_lifecycle_.load(models::ModelProfileKey{
      acquisition::ProfileKind::Safety, models::ModelTarget::SafetyB3});
  refresh_model_profiles();
  profile_generation_ = profile_manager_.generation();
  maintenance_router_.refresh_mode();
#ifdef ARDUINO
  ble_session_.set_calibration_backend(&ble_calibration_backend_);
#endif
  const acquisition::ConfigRef ref =
      acquisition::active_config_ref(config_manager_);
  const bool config_runtime_ready =
      config_manager_.runtime_ready() && ref.present;
  emit_runtime_startup(2U, config_runtime_ready ? 1 : 2, 2);
#ifdef ARDUINO
  log_runtime_diagnostic(RuntimeDiagnosticEvent::ConfigRuntimeReady,
                         config_runtime_ready,
                         static_cast<uint64_t>(esp_timer_get_time()));
#endif
  if (!config_runtime_ready) {
    acquisition_.disable();
    emit_runtime_startup(1U, 2, 2);
#ifdef ARDUINO
    log_runtime_diagnostic(RuntimeDiagnosticEvent::SetupComplete, false,
                           static_cast<uint64_t>(esp_timer_get_time()));
#endif
    return;
  }
  record_context_.config_ref = ref;
#ifdef ARDUINO
  profile_manager_.set_audit_context(acquisition::ArduinoMonotonicClock{}.now_us(), ref);
#else
  profile_manager_.set_audit_context(0U, ref);
#endif
  acquisition::DefaultProfiles default_profiles(profile_manager_, profile_store_);
  const auto default_result =
      default_profiles.bootstrap_enabled(sigap::build::kPhysicalReferenceOnly);
#ifdef ARDUINO
  Serial.printf(
      "[calibration-defaults] initialized=%u preserved=%u stored=%u blocked=%u errors=%u\n",
      static_cast<unsigned>(default_result.initialized),
      static_cast<unsigned>(default_result.preserved),
      static_cast<unsigned>(default_result.stored),
      static_cast<unsigned>(default_result.blocked),
      static_cast<unsigned>(default_result.errors));
#else
  (void)default_result;
#endif
  archive_runtime_.set_event_context(record_context_.device_id,
                                     record_context_.boot_id, ref);
#if SIGAP_PROTOTYPE_OPERATION
  network_worker_.emplace(
      network::NetworkPolicy{config_manager_.active().telemetry.retry_min_ms,
          config_manager_.active().telemetry.retry_max_ms, 10000U,
          config_manager_.active().telemetry.retry_max_ms, 250U},
      network_clock_, network_jitter_, network_link_, network_transport_,
      prototype_network_source_);
#else
  network_worker_.emplace(
      network::NetworkPolicy{config_manager_.active().telemetry.retry_min_ms,
          config_manager_.active().telemetry.retry_max_ms, 10000U,
          config_manager_.active().telemetry.retry_max_ms, 250U},
      network_clock_, network_jitter_, network_link_, network_transport_,
#ifdef ARDUINO
      async_network_source_);
  const bool ack_worker_started = async_network_source_.begin(
      static_cast<uint32_t>(xPortGetCoreID()));
  if (!ack_worker_started) Serial.println("[SIGAP][RUNTIME] ACK_WORKER_START=FAIL");
#else
      network_source_);
#endif
#endif
#ifdef ARDUINO
  // Persistent device-control result recovery must complete before the first
  // command fetch.  LittleFS is mounted without formatting; a failed mount
  // keeps device-control disabled while archive and acquisition remain safe.
  const bool littlefs_mounted = LittleFS.begin(false);
  log_runtime_diagnostic(RuntimeDiagnosticEvent::LittlefsMounted,
                         littlefs_mounted,
                         static_cast<uint64_t>(esp_timer_get_time()));
  bool filesystem_recovery_ready{SIGAP_PROTOTYPE_OPERATION != 0};
  if (!SIGAP_PROTOTYPE_OPERATION && storage::live_delivery::kPriorityFresh &&
      !live_paths_.valid) {
    Serial.println("[SIGAP][RUNTIME] LIVE_DELIVERY_PATHS=FAIL");
    acquisition_.disable();
    return;
  }
  if (littlefs_mounted && !SIGAP_PROTOTYPE_OPERATION) {
    const auto log_storage_stage = [](const char* stage, uint64_t started_us,
                                      bool ready) noexcept {
      const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
      const uint64_t elapsed_ms = now_us >= started_us
                                      ? (now_us - started_us) / 1000U
                                      : 0U;
      Serial.printf("[SIGAP][RUNTIME] %s=%s elapsed_ms=%llu\n", stage,
                    ready ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(elapsed_ms));
    };
    const uint64_t archive_started_us =
        static_cast<uint64_t>(esp_timer_get_time());
    Serial.println("[SIGAP][RUNTIME] LITTLEFS_ARCHIVE_RECOVERY=START");
    littlefs_store_.set_selection_context(
        record_context_.device_id, record_context_.boot_id,
        [](const storage::ImmutableRecordBytes& record, void* context) noexcept {
          auto& app = *static_cast<SigapApp*>(context);
          return app.legacy_eligibility_builder_.build(
              record, app.legacy_eligibility_scratch_);
        }, this);
    littlefs_store_.set_capacity_probe(
        [](uint64_t& available, void*) noexcept {
          size_t total{0U};
          size_t used{0U};
          if (esp_littlefs_info("spiffs", &total, &used) != ESP_OK ||
              total == 0U || used > total) return false;
          available = static_cast<uint64_t>(total - used);
          return true;
        }, nullptr);
    const bool archive_ready = littlefs_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes);
    log_storage_stage("LITTLEFS_ARCHIVE_RECOVERY", archive_started_us,
                      archive_ready);
#if SIGAP_LIVE_ARCHIVE_SPLIT
    Serial.printf("[SIGAP][RUNTIME] HISTORICAL_ARCHIVE_PRESERVED=%s bytes=%llu status=UNDELIVERED\n",
                  archive_ready ? "PASS" : "FAIL",
                  static_cast<unsigned long long>(littlefs_store_.retained_legacy_bytes()));
#endif
    const uint64_t batch_started_us =
        static_cast<uint64_t>(esp_timer_get_time());
    Serial.println("[SIGAP][RUNTIME] BATCH_RECOVERY=START");
    const bool batch_ready = batch_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes,
        storage::live_delivery::kPriorityFresh
            ? live_paths_.batches.data()
            : (build::kLiveArchiveSplit ? "/sigap/live-v3.batches"
                                        : "/sigap/batches.bin"),
        storage::live_delivery::kPriorityFresh
            ? live_paths_.batch_checkpoint.data()
            : (build::kLiveArchiveSplit ? "/sigap/live-v3.batches.ckp"
                                        : "/sigap/batches.ckp"));
    log_storage_stage("BATCH_RECOVERY", batch_started_us, batch_ready);
    const uint64_t ack_started_us = static_cast<uint64_t>(esp_timer_get_time());
    Serial.println("[SIGAP][RUNTIME] ACK_RECOVERY=START");
    const bool ack_ready = ack_journal_.begin(
        LittleFS,
        storage::live_delivery::kPriorityFresh
            ? live_paths_.acks.data()
            : (build::kLiveArchiveSplit ? "/sigap/live-v3.acks"
                                        : "/sigap/acks.bin"),
        storage::live_delivery::kPriorityFresh
            ? live_paths_.ack_checkpoint.data()
            : (build::kLiveArchiveSplit ? "/sigap/live-v3.acks.ckp"
                                        : "/sigap/acks.ckp"));
    log_storage_stage("ACK_RECOVERY", ack_started_us, ack_ready);
#if SIGAP_LIVE_ARCHIVE_SPLIT
    const bool live_ready = archive_ready && batch_ready && ack_ready &&
        batch_runtime_.recover_completed(legacy_eligibility_scratch_);
    Serial.printf("[SIGAP][RUNTIME] LIVE_DELIVERY_RECOVERY=%s\n", live_ready ? "PASS" : "FAIL");
    if (!live_ready) batch_runtime_.bind(nullptr, &batch_store_, &ack_journal_);
#endif
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    const uint64_t journal_started_us =
        static_cast<uint64_t>(esp_timer_get_time());
    Serial.println("[SIGAP][RUNTIME] RESULT_JOURNAL_RECOVERY=START");
    device_control_journal_ready_ =
        device_control_result_journal_.begin(LittleFS) &&
        device_control_result_journal_.recover();
    log_storage_stage("RESULT_JOURNAL_RECOVERY", journal_started_us,
                      device_control_journal_ready_);
#endif
    filesystem_recovery_ready = archive_ready && batch_ready && ack_ready;
  }
  if (SIGAP_PROTOTYPE_OPERATION && littlefs_mounted) {
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    device_control_journal_ready_ =
        device_control_result_journal_.begin(LittleFS) &&
        device_control_result_journal_.recover();
#endif
  }
  log_runtime_diagnostic(RuntimeDiagnosticEvent::FilesystemRecovery,
                         filesystem_recovery_ready,
                         static_cast<uint64_t>(esp_timer_get_time()));
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  log_runtime_diagnostic(RuntimeDiagnosticEvent::JournalReady,
                         device_control_journal_ready_,
                         static_cast<uint64_t>(esp_timer_get_time()));
#endif
#else
  (void)littlefs_store_.begin(
      config_manager_.active().storage.flash_pending_quota_bytes);
  (void)sd_store_.begin(
      config_manager_.active().storage.sd_pending_quota_bytes);
#endif
#if SIGAP_PROTOTYPE_OPERATION
#if SIGAP_LIVE_NO_BACKLOG
  archive_runtime_.set_store(nullptr);
#else
  prototype_batch_runtime_.bind(&prototype_archive_store_);
  archive_runtime_.set_store(&prototype_archive_store_);
#endif
  archive_runtime_.set_media_migration(nullptr);
#endif
  // Network credentials are loaded only into the worker boundary. They are
  // never serialized, logged, or exposed through maintenance responses.
  provisioning::NetworkSecrets network_secrets{};
  const bool network_secrets_loaded =
      secret_store_.load_for_network(network_secrets);
#ifdef ARDUINO
  log_runtime_diagnostic(RuntimeDiagnosticEvent::NetworkSecretsLoaded,
                         network_secrets_loaded,
                         static_cast<uint64_t>(esp_timer_get_time()));
#endif
  if (network_secrets_loaded) {
    std::size_t token_size{0U};
    while (token_size < network_secrets.device_token.size() &&
           network_secrets.device_token[token_size] != '\0') {
      ++token_size;
    }
#ifdef ARDUINO
    bool transport_ready{false};
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    transport_ready = network_transport_.set_endpoint(SIGAP_NETWORK_ENDPOINT) &&
                      network_transport_.begin(network_secrets,
                                               SIGAP_NETWORK_CA_CERT);
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    // The shared endpoint carries the ingest path; the control client needs the
    // device-control route on the same origin.
    constexpr std::string_view kIngestPath{"/api/telemetry/batches"};
    constexpr std::string_view kControlPath{"/api/device-control/device"};
    const std::string_view shared_endpoint{SIGAP_NETWORK_ENDPOINT};
    char control_endpoint[192]{};
    bool control_endpoint_ready{false};
    if (shared_endpoint.size() > kIngestPath.size() &&
        shared_endpoint.substr(shared_endpoint.size() - kIngestPath.size()) ==
            kIngestPath) {
      const std::string_view origin =
          shared_endpoint.substr(0, shared_endpoint.size() - kIngestPath.size());
      const int written = std::snprintf(
          control_endpoint, sizeof(control_endpoint), "%.*s%s",
          static_cast<int>(origin.size()), origin.data(), kControlPath.data());
      control_endpoint_ready =
          written > 0 && static_cast<std::size_t>(written) < sizeof(control_endpoint);
    }
    const bool control_transport_ready =
        control_endpoint_ready &&
        device_control_transport_.set_endpoint(std::string_view(control_endpoint)) &&
        device_control_transport_.begin(SIGAP_NETWORK_CA_CERT);
    device_control_ready_ = control_transport_ready &&
                            device_control_journal_ready_ &&
                            device_control_poller_.begin(
                                std::string_view(device_control_id_.data()),
                                std::string_view(network_secrets.device_token.data(),
                                                 token_size));
#endif
#endif
#else
    constexpr bool transport_ready{true};
#endif
    const bool network_begin_accepted = transport_ready && token_size != 0U &&
                     network_worker_->begin(std::string_view(
                         network_secrets.device_token.data(), token_size));
#ifdef ARDUINO
    network_ready_ = network_begin_accepted;
#else
    network_ready_ = network_begin_accepted;
#endif
#if defined(ARDUINO) && defined(SIGAP_NETWORK_CA_CERT)
    const auto& debug_config = build::kDebugUploadConfig;
    if (debug_config.available() && SIGAP_NETWORK_CA_CERT[0] != '\0') {
      debug_transport_.emplace(debug_config.endpoint, SIGAP_NETWORK_CA_CERT);
      debug_worker_.emplace(debug_service_, *debug_transport_, io_gate_,
                            debug_config.device_code);
      if (!debug_worker_->configure(debug_config.endpoint,
              std::string_view(network_secrets.device_token.data(), token_size))) {
        debug_worker_.reset();
        debug_transport_.reset();
      }
    }
    if (network_ready_ || debug_worker_.has_value()) {
      // TLS certificate validity cannot be checked against the reset epoch.
      // SNTP starts asynchronously; loop() gates every HTTPS worker until a
      // trustworthy wall-clock value is available.
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    }
#endif
    provisioning::secure_zero(network_secrets.ssid.data(),
                              network_secrets.ssid.size());
    provisioning::secure_zero(network_secrets.password.data(),
                              network_secrets.password.size());
    provisioning::secure_zero(network_secrets.device_token.data(),
                              network_secrets.device_token.size());
  }
#ifdef ARDUINO
  log_runtime_diagnostic(RuntimeDiagnosticEvent::NetworkReady, network_ready_,
                         static_cast<uint64_t>(esp_timer_get_time()));
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  log_runtime_diagnostic(RuntimeDiagnosticEvent::DeviceControlReady,
                         device_control_ready_,
                         static_cast<uint64_t>(esp_timer_get_time()));
#endif
#endif
#ifdef ARDUINO
#ifdef SIGAP_USE_SD
  // SD_MMC probing is intentionally lazy.  Card initialization and archive
  // recovery can block for an unbounded media timeout, so doing them in setup
  // would prevent USB maintenance, BLE, WiFi, and HTTPS from reaching their
  // bounded event loops.  The owned storage probe performs the same mount and
  // archive recovery before any SD test-owned write.
#endif
#endif

#if SIGAP_SENSOR_SIMULATION
  if (!acquisition::make_simulated_ds18b20_profile(simulated_ds_profile_)) {
    acquisition_.disable();
    emit_runtime_startup(1U, 2, 3);
    return;
  }
  profiles_.ds18b20 = &simulated_ds_profile_;
#endif
  const processing::ResultRecordContext result_context{
      record_context_.device_id,
      record_context_.boot_id,
      ref,
      record_context_.utc_source,
  };
#ifdef ARDUINO
  uint64_t now_us{0U};
  const acquisition::ArduinoMonotonicClock clock{};
  now_us = clock.now_us();
  emit_runtime_startup(2U, 0, 3);
#if !SIGAP_PROTOTYPE_OPERATION
  const bool processing_started = processing_runtime_.begin(
      config_manager_.active(), ref, boot_identity_.sequence, result_context,
      profiles_, now_us);
#else
  const bool processing_started = true;
#endif
  const bool acquisition_started = acquisition_.begin(
      config_manager_.active(), ref, now_us);
#else
  const uint64_t now_us{0U};
  emit_runtime_startup(2U, 0, 3);
  const bool acquisition_started = acquisition_.begin(config_manager_.active(), ref, 0U);
#if !SIGAP_PROTOTYPE_OPERATION
  const bool processing_started = processing_runtime_.begin(
      config_manager_.active(), ref, boot_identity_.sequence, result_context,
      profiles_, 0U);
#else
  const bool processing_started = true;
#endif
#endif
  const bool critical_startup_ready = config_runtime_ready &&
                                      acquisition_started && processing_started;
  emit_runtime_startup(2U, critical_startup_ready ? 1 : 2, 3);
  if (record_context_.device_id.present) {
#if SIGAP_PROTOTYPE_OPERATION && SIGAP_LIVE_NO_BACKLOG
    (void)prototype_batch_runtime_.begin(record_context_, boot_identity_.sequence,
        now_us, build::kFirmwareBuildId,
#ifdef ARDUINO
        reset_reason_code()
#else
        0U
#endif
    );
#else
    const auto metadata_status = archive_runtime_.append_boot_metadata(
        now_us, build::kFirmwareBuildId,
#ifdef ARDUINO
        reset_reason_code()
#else
        0U
#endif
    );
    // Keep filesystem I/O out of setup and the USB commissioning boundary.
    // The owned storage worker drains this queued metadata record from the
    // bounded loop slice after all runtime services are available.
    (void)metadata_status;
#endif
  }
#ifdef ARDUINO
  log_runtime_diagnostic(RuntimeDiagnosticEvent::SetupComplete, true,
                         static_cast<uint64_t>(esp_timer_get_time()));
#endif
  emit_runtime_startup(1U, critical_startup_ready ? 1 : 2,
                       critical_startup_ready ? 1 : 3);
#if SIGAP_PROTOTYPE_OPERATION
#if !SIGAP_LIVE_NO_BACKLOG
  prototype_batch_runtime_.bind(&prototype_archive_store_);
  const acquisition::InjectedMonotonicClock prototype_debug_clock{
      [](void*) noexcept { return acquisition::ArduinoMonotonicClock{}.now_us(); }, nullptr};
  prototype_batch_runtime_.bind_debug(&debug_service_, &prototype_debug_clock);
  // The bounded accumulation window needs the real monotonic clock; the RAM
  // drain and batch poll stay loop-owned and are not gated on filesystem I/O.
  prototype_batch_runtime_.bind_clock(prototype_debug_clock);
  // Prototype live records use volatile RAM only; historical LittleFS data stays isolated.
  archive_runtime_.set_store(&prototype_archive_store_);
  archive_runtime_.set_media_migration(nullptr);
#endif
  emit_runtime_startup(3U, 1, 1);
#endif
}

// cppcheck-suppress unusedFunction
void SigapApp::observe_loop_gap(uint64_t now_us) noexcept {
  using runtime_debug_policy::LoopGapClass;
  uint64_t gap_us{0U};
  LoopGapClass classification{LoopGapClass::Healthy};
  if (loop_gap_baseline_valid_) {
    if (now_us < previous_loop_entry_us_) {
      previous_loop_entry_us_ = now_us;
      return;
    }
    gap_us = now_us - previous_loop_entry_us_;
    classification = runtime_debug_policy::classify_loop_gap(gap_us);
  } else {
    loop_gap_baseline_valid_ = true;
  }
  previous_loop_entry_us_ = now_us;
  const auto wire_class = static_cast<uint8_t>(classification);
  if (last_loop_gap_class_ == wire_class) return;
  last_loop_gap_class_ = wire_class;
  debug::EventInput event{};
  event.module = debug::Module::Runtime;
  event.event_code = 3U;
  event.level = debug::Level::Debug;
  event.arg0 = static_cast<int32_t>(wire_class);
  event.arg1 = gap_us > static_cast<uint64_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(gap_us);
  event.monotonic_us = now_us;
  (void)debug_service_.emit(event);
}

void SigapApp::loop() noexcept {
#ifdef ARDUINO
  acquisition::usb_phase_begin_loop();
  const acquisition::ArduinoMonotonicClock clock{};
  const uint64_t now_us = clock.now_us();
  observe_loop_gap(now_us);
  log_runtime_diagnostic(RuntimeDiagnosticEvent::LoopAlive, true, now_us);
  acquisition::usb_phase_mark(acquisition::UsbPhase::Wifi);
  provisioning_clock_.set_now(now_us);
  if (!io_gate_.active(debug::IoOwner::DebugHttp)) wifi_manager_.poll();
  const bool wifi_connected =
      wifi_manager_.state() == provisioning::WifiState::Connected;
  if (wifi_connected || now_us >= runtime_diagnostics_started_us_ + 5000000U) {
    log_runtime_diagnostic(RuntimeDiagnosticEvent::WifiConnected,
                           wifi_connected, now_us);
  }
  acquisition::usb_phase_mark(acquisition::UsbPhase::Ble);
  ble_service_.poll();
  acquisition::usb_phase_mark(acquisition::UsbPhase::Maintenance);
  maintenance_service_.poll(now_us);
  acquisition::usb_phase_mark(acquisition::UsbPhase::Filesystem);
  const bool filesystem_owned = io_gate_.try_acquire(debug::IoOwner::Filesystem);
  std::size_t audit_budget{2U};
  while (!SIGAP_LIVE_NO_BACKLOG && filesystem_owned && audit_budget-- != 0U &&
         archive_runtime_.drain_audit_events(audit_ring_)) {
  }
  acquisition::usb_phase_mark(acquisition::UsbPhase::Profiles);
  refresh_model_profiles();
  if (profile_generation_ != profile_manager_.generation()) {
    profile_generation_ = profile_manager_.generation();
    // Apply profile objects at the channel boundary without rebuilding the
    // scheduler or restarting unrelated sensors.
    (void)acquisition_.refresh_profiles(now_us);
  }
  acquisition::usb_phase_mark(acquisition::UsbPhase::Acquisition);
  acquisition_.poll(now_us);
#ifdef ARDUINO
  acquisition::usb_phase_mark(acquisition::UsbPhase::Emitter);
  const int64_t usb_sensor_time = esp_timer_get_time();
  acquisition_.target().emit_usb_debug(usb_sensor_time < 0 ? 0U : static_cast<uint64_t>(usb_sensor_time));
#endif
  acquisition::usb_phase_mark(acquisition::UsbPhase::Drain);
#if SIGAP_PROTOTYPE_OPERATION && SIGAP_LIVE_NO_BACKLOG
  acquisition::SampleRecord current_sample{};
  while (acquisition_.handoff().try_pop(current_sample) == acquisition::HandoffStatus::Dequeued)
    prototype_batch_runtime_.observe(current_sample);
  acquisition_.handoff().clear_last_dropped();
#endif
#if !SIGAP_PROTOTYPE_OPERATION
  processing_runtime_.poll(now_us);
#endif
#if SIGAP_PROTOTYPE_OPERATION && !SIGAP_LIVE_NO_BACKLOG
  (void)archive_runtime_.poll_storage_slice(4U, 32U * 1024U);
  (void)prototype_batch_runtime_.poll();
#endif
  if (filesystem_owned) {
#if !SIGAP_PROTOTYPE_OPERATION
    if (samples_first_drain_) {
      (void)archive_runtime_.drain_samples(acquisition_.handoff());
      (void)archive_runtime_.drain_results(processing_runtime_.handoff());
    } else {
      (void)archive_runtime_.drain_results(processing_runtime_.handoff());
      (void)archive_runtime_.drain_samples(acquisition_.handoff());
    }
    samples_first_drain_ = !samples_first_drain_;
#endif
  }
#if SIGAP_PROTOTYPE_OPERATION && !SIGAP_LIVE_NO_BACKLOG
  // The legacy prototype sample handoff and RAM archive are loop-owned; network I/O
  // ownership must not pause draining while HTTPS is in flight.
  (void)archive_runtime_.drain_samples(acquisition_.handoff());
#endif
  // LittleFS and the Arduino VFS share the global descriptor table with the
  // asynchronous HTTPS workers. Keep filesystem ownership in loopTask while
  // either transport task is still active; this prevents a failed TLS socket
  // teardown from colliding with an in-flight filesystem descriptor.
  bool filesystem_transport_busy{false};
  if (network_worker_.has_value() && network_worker_->busy()) {
    filesystem_transport_busy = true;
  }
#ifdef ARDUINO
  if (!SIGAP_PROTOTYPE_OPERATION && async_network_source_.busy()) {
    filesystem_transport_busy = true;
  }
#endif
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  if (device_control_ready_ && device_control_poller_.busy()) {
    filesystem_transport_busy = true;
  }
#endif
  if (filesystem_owned && !filesystem_transport_busy &&
      !SIGAP_PROTOTYPE_OPERATION) {
    acquisition::usb_phase_mark(acquisition::UsbPhase::StorageSlice);
    (void)archive_runtime_.poll_storage_slice(4U, 32U * 1024U);
    acquisition::usb_phase_mark(acquisition::UsbPhase::StorageBatch);
    (void)batch_runtime_.poll();
    // ACK/reclaim runs on the other core. Read mutable store counters only
    // while this task holds exclusive filesystem ownership.
    last_flow_pending_bytes_ = littlefs_store_.pending_bytes();
    if (!legacy_retention_reported_ && littlefs_store_.retained_records() != 0U) {
      Serial.printf("[SIGAP][RUNTIME] LEGACY_PENDING_RETAINED=DEGRADED observed_records=%llu observed_bytes=%llu originals=UNDELIVERED\n",
                    static_cast<unsigned long long>(littlefs_store_.retained_records()),
                    static_cast<unsigned long long>(littlefs_store_.retained_bytes()));
      legacy_retention_reported_ = true;
    }
  }
  acquisition::usb_phase_mark(acquisition::UsbPhase::Migration);
  if (filesystem_owned) {
    if (!SIGAP_PROTOTYPE_OPERATION) {
      (void)archive_runtime_.poll_media_migration();
    }
    io_gate_.release(debug::IoOwner::Filesystem);
  }
#ifdef ARDUINO
#if SIGAP_PROTOTYPE_OPERATION && SIGAP_LIVE_NO_BACKLOG
  if (now_us >= last_flow_diagnostic_us_ + 1000000U) {
    char frame[640U]{};
    const int length = std::snprintf(frame, sizeof(frame),
        "SIGAP_LIVE v=32 mode=1 backlog=0 period_us=%llu current=%u in_flight=%u observed=%llu superseded=%llu expired=%llu rejected=%llu unconfirmed=%llu ack_batches=%llu ack_records=%llu start_age_us=%llu uptime_us=%llu loops=%lu heap=%lu min_heap=%lu stack_hwm=%lu outcome=%u handoff_dropped=%llu\n",
        static_cast<unsigned long long>(build::kLiveSnapshotPeriodUs),
        static_cast<unsigned>(prototype_batch_runtime_.retained()),
        prototype_batch_runtime_.pending() ? 1U : 0U,
        static_cast<unsigned long long>(prototype_batch_runtime_.observed()),
        static_cast<unsigned long long>(prototype_batch_runtime_.superseded()),
        static_cast<unsigned long long>(prototype_batch_runtime_.expired()),
        static_cast<unsigned long long>(prototype_batch_runtime_.rejected()),
        static_cast<unsigned long long>(prototype_batch_runtime_.discarded_records()),
        static_cast<unsigned long long>(prototype_batch_runtime_.batches_delivered()),
        static_cast<unsigned long long>(prototype_batch_runtime_.records_delivered()),
        static_cast<unsigned long long>(prototype_batch_runtime_.last_start_age_us()),
        static_cast<unsigned long long>(now_us),
        static_cast<unsigned long>(acquisition::usb_phase_state().loops),
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(ESP.getMinFreeHeap()),
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)),
        static_cast<unsigned>(last_network_outcome_),
        static_cast<unsigned long long>(acquisition_.handoff().dropped_records()));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(frame) &&
        Serial.availableForWrite() >= length)
      (void)Serial.write(reinterpret_cast<const uint8_t*>(frame), static_cast<std::size_t>(length));
    last_flow_diagnostic_us_ = now_us;
  }
#elif SIGAP_PROTOTYPE_OPERATION
  if (now_us >= last_flow_diagnostic_us_ + 1000000U) {
    char frame[768U]{};
    const auto stats = archive_runtime_.storage_worker().stats();
    // Batches and records are reported separately: `batches_delivered` counts
    // whole ACKed batches while `records_delivered` counts their records.
    const int length = std::snprintf(frame, sizeof(frame),
        "SIGAP_FLOW prototype ram_records=%u pending_bytes=%llu queued_records=%u retryable=%llu terminal=%llu overflow=%llu handoff_dropped=%llu outcome=%u polls=%llu network_delivered_batches=%llu batches_built=%llu batches_delivered=%llu records_delivered=%llu batch_records=%u batch_reclaimed=%u source_mismatch=%llu partial_reclaim=%llu\n",
        static_cast<unsigned>(prototype_archive_store_.size()),
        static_cast<unsigned long long>(prototype_archive_store_.pending_bytes()),
        static_cast<unsigned>(archive_runtime_.queued_records()),
        static_cast<unsigned long long>(stats.retryable),
        static_cast<unsigned long long>(stats.terminal),
        static_cast<unsigned long long>(stats.overflow),
        static_cast<unsigned long long>(acquisition_.handoff().dropped_records()),
        static_cast<unsigned>(last_network_outcome_),
        static_cast<unsigned long long>(network_poll_count_),
        static_cast<unsigned long long>(network_delivered_count_),
        static_cast<unsigned long long>(prototype_batch_runtime_.batches_built()),
        static_cast<unsigned long long>(prototype_batch_runtime_.batches_delivered()),
        static_cast<unsigned long long>(prototype_batch_runtime_.records_delivered()),
        static_cast<unsigned>(prototype_batch_runtime_.manifest_size()),
        static_cast<unsigned>(prototype_batch_runtime_.reclaimed_size()),
        static_cast<unsigned long long>(prototype_batch_runtime_.source_mismatch_count()),
        static_cast<unsigned long long>(prototype_batch_runtime_.partial_reclaim_count()));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(frame) &&
        Serial.availableForWrite() >= length) {
      (void)Serial.write(reinterpret_cast<const uint8_t*>(frame),
                         static_cast<std::size_t>(length));
    }
    last_flow_diagnostic_us_ = now_us;
  }
#else
  if (now_us >= last_flow_diagnostic_us_ + 1000000U) {
    char frame[640U]{};
    const auto boot_word = [this](std::size_t offset) noexcept {
      const auto& bytes = record_context_.boot_id.bytes;
      return (static_cast<uint32_t>(bytes[offset]) << 24U) |
             (static_cast<uint32_t>(bytes[offset + 1U]) << 16U) |
             (static_cast<uint32_t>(bytes[offset + 2U]) << 8U) |
             static_cast<uint32_t>(bytes[offset + 3U]);
    };
#if SIGAP_LIVE_ARCHIVE_SPLIT
    const uint64_t historical_bytes = littlefs_store_.retained_legacy_bytes();
#else
    const uint64_t historical_bytes = 0U;
#endif
    const int length = std::snprintf(
        frame, sizeof(frame),
        "SIGAP_FLOW v=14 utc_ready=%u bp=%u b0=%lu b1=%lu b2=%lu b3=%lu enabled=%u cycles=%llu observed=%llu rejected=%llu result_failures=%llu results_pending=%u result_drops=%llu encode_failures=%llu durable=%llu queued=%llu sample_pending=%u sample_drops=%llu fs_owned=%u transport_busy=%u priority_fresh=%u historical_bytes=%llu pending_bytes=%llu hxq=%u mqq=%u shtq=%u usq=%u\n",
        utc_source_.ready() ? 1U : 0U,
        record_context_.boot_id.present ? 1U : 0U,
        static_cast<unsigned long>(boot_word(0U)),
        static_cast<unsigned long>(boot_word(4U)),
        static_cast<unsigned long>(boot_word(8U)),
        static_cast<unsigned long>(boot_word(12U)),
        processing_runtime_.enabled() ? 1U : 0U,
        static_cast<unsigned long long>(processing_runtime_.cycles_completed()),
        static_cast<unsigned long long>(processing_runtime_.observed_samples()),
        static_cast<unsigned long long>(processing_runtime_.rejected_samples()),
        static_cast<unsigned long long>(processing_runtime_.result_failures()),
        static_cast<unsigned>(processing_runtime_.handoff().size()),
        static_cast<unsigned long long>(processing_runtime_.handoff().dropped_results()),
        static_cast<unsigned long long>(archive_runtime_.encode_failures()),
        static_cast<unsigned long long>(archive_runtime_.durable_records()),
        static_cast<unsigned long long>(archive_runtime_.queued_records()),
        static_cast<unsigned>(acquisition_.handoff().size()),
        static_cast<unsigned long long>(acquisition_.handoff().dropped_records()),
        filesystem_owned ? 1U : 0U,
        filesystem_transport_busy ? 1U : 0U,
        storage::live_delivery::kPriorityFresh ? 1U : 0U,
        static_cast<unsigned long long>(historical_bytes),
        static_cast<unsigned long long>(last_flow_pending_bytes_),
        static_cast<unsigned>(acquisition_.handoff().pending_for(acquisition::SensorId::Hx711Organic)),
        static_cast<unsigned>(acquisition_.handoff().pending_for(acquisition::SensorId::Mq135)),
        static_cast<unsigned>(acquisition_.handoff().pending_for(acquisition::SensorId::Sht31_44)),
        static_cast<unsigned>(acquisition_.handoff().pending_for(acquisition::SensorId::UltrasonicOrganic)));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(frame) &&
        Serial.availableForWrite() >= length) {
      (void)Serial.write(reinterpret_cast<const uint8_t*>(frame),
                         static_cast<std::size_t>(length));
    }
    last_flow_diagnostic_us_ = now_us;
  }
#endif
#endif
  network_clock_.set_now(now_us);
  bool tls_time_ready{true};
#ifdef SIGAP_NETWORK_CA_CERT
  const std::time_t wall_clock = std::time(nullptr);
  network_time_ready_ = wall_clock >= static_cast<std::time_t>(1700000000);
  tls_time_ready = network_time_ready_;
  if (network_time_ready_ ||
      now_us >= runtime_diagnostics_started_us_ + 30000000U) {
    log_runtime_diagnostic(RuntimeDiagnosticEvent::SntpTimeReady,
                           network_time_ready_, now_us);
  }
#endif
  acquisition::usb_phase_mark(acquisition::UsbPhase::ArchiveHttp);
  if (network_worker_.has_value() &&
      (network_worker_->busy() || (network_ready_ && tls_time_ready
#ifdef ARDUINO
#if SIGAP_PROTOTYPE_OPERATION
                                  && network_ready_
#else
                                  && async_network_source_.ready()
#endif
#endif
                                  )) &&
      (io_gate_.active(debug::IoOwner::ArchiveHttp) ||
       io_gate_.try_acquire(debug::IoOwner::ArchiveHttp))) {
    log_runtime_diagnostic(RuntimeDiagnosticEvent::HttpsPollEntered, true,
                           now_us);
#if SIGAP_PROTOTYPE_OPERATION && SIGAP_LIVE_NO_BACKLOG
    if (!network_worker_->busy() && wifi_connected && wifi_station_.connected() &&
        now_us >= network_worker_->next_attempt_us() &&
        network_worker_->state() != network::NetworkState::AuthRequired)
      (void)prototype_batch_runtime_.prepare(acquisition::ArduinoMonotonicClock{}.now_us());
#endif
    const auto outcome = network_worker_->poll();
#if SIGAP_PROTOTYPE_OPERATION && SIGAP_LIVE_NO_BACKLOG
    if (!network_worker_->busy() && prototype_batch_runtime_.pending() &&
        outcome != network::NetworkOutcome::Waiting && outcome != network::NetworkOutcome::Idle &&
        outcome != network::NetworkOutcome::Delivered)
      prototype_batch_runtime_.discard_unconfirmed();
#endif
#if SIGAP_PROTOTYPE_OPERATION
    last_network_outcome_ = outcome;
    ++network_poll_count_;
    if (outcome == network::NetworkOutcome::Delivered) ++network_delivered_count_;
#endif
    if (!network_worker_->busy()
#ifdef ARDUINO
#if SIGAP_PROTOTYPE_OPERATION
#else
        && !async_network_source_.busy()
#endif
#endif
        ) io_gate_.release(debug::IoOwner::ArchiveHttp);
  }
#ifdef ARDUINO
  if (now_us >= last_ack_worker_diagnostic_us_ + 1000000U) {
#if SIGAP_PROTOTYPE_OPERATION
    if (network_worker_.has_value()) {
      char frame[192U]{};
      const int length = std::snprintf(frame, sizeof(frame),
          "SIGAP_ACKWORK path=prototype_main_loop ready=%u busy=%u ack_pending=%u retry_attempt=%lu\n",
          network_ready_ ? 1U : 0U, network_worker_->busy() ? 1U : 0U,
          network_worker_->ack_pending() ? 1U : 0U,
          static_cast<unsigned long>(network_worker_->retry_attempt()));
      if (length > 0 && static_cast<std::size_t>(length) < sizeof(frame) &&
          Serial.availableForWrite() >= length) {
        (void)Serial.write(reinterpret_cast<const uint8_t*>(frame),
                           static_cast<std::size_t>(length));
      }
    }
#else
    const auto snapshot = async_network_source_.snapshot();
    if (snapshot.valid) {
      char frame[192U]{};
      const int length = std::snprintf(frame, sizeof(frame),
          "SIGAP_ACKWORK v=9 main=%u worker=%ld ready=%u busy=%u submitted=%lu finished=%lu duration_us=%llu stack_hwm=%lu\n",
          static_cast<unsigned>(xPortGetCoreID()),
          static_cast<long>(snapshot.worker_core), snapshot.ready ? 1U : 0U,
          snapshot.busy ? 1U : 0U,
          static_cast<unsigned long>(snapshot.started),
          static_cast<unsigned long>(snapshot.completed),
          static_cast<unsigned long long>(snapshot.last_duration_us),
          static_cast<unsigned long>(snapshot.stack_hwm));
      if (length > 0 && static_cast<std::size_t>(length) < sizeof(frame) &&
          Serial.availableForWrite() >= length) {
        (void)Serial.write(reinterpret_cast<const uint8_t*>(frame),
                           static_cast<std::size_t>(length));
      }
    }
#endif
    last_ack_worker_diagnostic_us_ = now_us;
  }
#endif
  // The shared controller owns session expiry for every transport, including
  // BLE and USB builds that do not enable the optional HTTPS device-control
  // endpoint.
  acquisition::usb_phase_mark(acquisition::UsbPhase::Calibration);
  calibration_controller_.poll(now_us);
  acquisition::usb_phase_mark(acquisition::UsbPhase::ControlHttp);
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  control_transport_boundary_.set_time_valid(tls_time_ready);
  if (device_control_ready_ && (tls_time_ready || device_control_poller_.busy()) &&
      (io_gate_.active(debug::IoOwner::ControlHttp) ||
       io_gate_.try_acquire(debug::IoOwner::ControlHttp))) {
    log_runtime_diagnostic(RuntimeDiagnosticEvent::HttpsPollEntered, true,
                           now_us);
    (void)device_control_poller_.poll();
    if (!device_control_poller_.busy()) io_gate_.release(debug::IoOwner::ControlHttp);
  }
#endif
  acquisition::usb_phase_mark(acquisition::UsbPhase::DebugWorker);
  if (debug_worker_.has_value()) {
    bool ca_ready{false};
#ifdef SIGAP_NETWORK_CA_CERT
    ca_ready = SIGAP_NETWORK_CA_CERT[0] != '\0';
#endif
    debug_worker_->poll(now_us, debug::DebugReadiness{
        wifi_connected && wifi_station_.connected(), network_time_ready_, ca_ready,
        config_manager_.runtime_ready() && build::kDebugUploadConfig.available(),
        io_gate_.active(debug::IoOwner::ControlHttp) || calibration_controller_.active(),
        io_gate_.active(debug::IoOwner::ArchiveHttp)});
  }
  maintenance_service_.poll(now_us);
  acquisition::usb_phase_mark(acquisition::UsbPhase::UsbSink);
  debug_usb_sink_.poll(now_us, 384U);
#ifdef ARDUINO
  acquisition::usb_phase_mark(acquisition::UsbPhase::Idle);
  if (reboot_controller_.consume_request()) {
    ESP.restart();
  }
#endif
#endif
}

}  // namespace sigap
