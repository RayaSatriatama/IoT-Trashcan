#include "maintenance/storage_probe_response.h"
#include "sigap_app.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <string_view>

#include "sigap/board/board_memory.h"
#include "sigap/board/sd_transport.h"
#include "sigap/build/build_config.h"
#include "acquisition/provenance.h"
#include "acquisition/arduino_adapters.h"
#include "storage/sd_probe_path.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <LittleFS.h>
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
                          &sd_probe_, &ble_service_),
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
          network_clock_, device_control_transport_, device_control_executor_,
          &device_control_result_journal_),
#endif
#endif
      network_source_(batch_runtime_),
#ifdef ARDUINO
      wifi_manager_(secret_store_, wifi_station_, provisioning_clock_),
      wifi_backend_(wifi_manager_),
      ble_session_(wifi_backend_, provisioning_clock_),
#endif
      maintenance_service_(maintenance_transport_, maintenance_router_) {
#if defined(ARDUINO) && defined(SIGAP_NETWORK_ENDPOINT) && \
    defined(SIGAP_NETWORK_CA_CERT)
  network_link_.bind(&network_transport_);
#endif
#ifdef ARDUINO
  maintenance_router_.set_calibration_transport_controller(
      &calibration_controller_);
#endif
  sd_probe_.bind_archive_store(
      sd_store_, config_manager_.active().storage.sd_pending_quota_bytes);
  profile_manager_.bind_runtime_profiles(profiles_);
  evidence_cache_.set_profiles(&profiles_);
  archive_runtime_.set_store(&archive_store_);
  archive_runtime_.set_media_migration(&media_migration_);
  archive_runtime_.set_sequence_allocator(&boot_identity_.sequence);
  batch_runtime_.bind(&archive_store_, &batch_store_, &ack_journal_);
  archive_runtime_.set_event_context(record_context_.device_id,
                                     record_context_.boot_id,
                                     record_context_.config_ref);
  processing_runtime_.set_downstream_observer(&evidence_cache_);
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
  for (std::size_t index = 0U; index < fis_keys.size(); ++index) {
    snapshot = models::ModelProfileSnapshot{};
    if (model_lifecycle_.snapshot(fis_keys[index], snapshot) && snapshot.present &&
        models::ModelProfilePersistence::default_codec().decode(
            snapshot.encoded, *fis_models[index])) {
      fis_refs[index] = fis_models[index];
      fis_model_present_ = true;
    }
  }
  snapshot = models::ModelProfileSnapshot{};
  if (model_lifecycle_.snapshot(safety_key, snapshot) && snapshot.present) {
    safety_model_present_ = models::ModelProfilePersistence::default_codec().decode(
        snapshot.encoded, safety_model_);
  }
  processing_runtime_.set_model_profiles(
      fis_refs,
      safety_model_present_ ? &safety_model_ : nullptr);
}

// cppcheck-suppress unusedFunction
void SigapApp::setup() noexcept {
#ifdef ARDUINO
  Serial.begin(115200);
  board::log_runtime_board_memory();
  const int64_t started_us = esp_timer_get_time();
  provisioning_clock_.set_now(
      started_us < 0 ? 0U : static_cast<uint64_t>(started_us));
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
  (void)std::snprintf(device_control_id_.data(), device_control_id_.size(),
                      "%llX", static_cast<unsigned long long>(device_mac));
  calibration_controller_.set_device_id(device_mac);
  calibration_authorizer_.set_device_id(device_mac);
  device_control_transport_.bind_clock(network_clock_);
#endif
  (void)ble_service_.begin(ble_session_, std::string_view(ble_name));
#endif
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
  if (!config_manager_.runtime_ready()) {
    acquisition_.disable();
    return;
  }
  const acquisition::ConfigRef ref =
      acquisition::active_config_ref(config_manager_);
  if (!ref.present) {
    acquisition_.disable();
    return;
  }
  record_context_.config_ref = ref;
  archive_runtime_.set_event_context(record_context_.device_id,
                                     record_context_.boot_id, ref);
  network_worker_.emplace(
      network::NetworkPolicy{
          config_manager_.active().telemetry.retry_min_ms,
          config_manager_.active().telemetry.retry_max_ms,
          10000U,
          config_manager_.active().telemetry.retry_max_ms,
          250U},
      network_clock_, network_jitter_, network_link_, network_transport_,
      network_source_);
#ifdef ARDUINO
  // Persistent device-control result recovery must complete before the first
  // command fetch.  LittleFS is mounted without formatting; a failed mount
  // keeps device-control disabled while archive and acquisition remain safe.
  const bool littlefs_mounted = LittleFS.begin(false);
  if (littlefs_mounted) {
    (void)migration_journal_.begin(LittleFS);
    (void)littlefs_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes);
    (void)batch_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes);
    (void)ack_journal_.begin(LittleFS);
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    device_control_journal_ready_ =
        device_control_result_journal_.begin(LittleFS) &&
        device_control_result_journal_.recover();
#endif
  }
#else
  (void)littlefs_store_.begin(
      config_manager_.active().storage.flash_pending_quota_bytes);
  (void)sd_store_.begin(
      config_manager_.active().storage.sd_pending_quota_bytes);
#endif
  // Network credentials are loaded only into the worker boundary. They are
  // never serialized, logged, or exposed through maintenance responses.
  provisioning::NetworkSecrets network_secrets{};
  if (secret_store_.load_for_network(network_secrets)) {
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
    const bool control_transport_ready =
        device_control_transport_.set_endpoint(SIGAP_NETWORK_ENDPOINT) &&
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
    network_ready_ = transport_ready && token_size != 0U &&
                     network_worker_->begin(std::string_view(
                         network_secrets.device_token.data(), token_size));
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
    if (network_ready_) {
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
#ifdef SIGAP_USE_SD
  // SD_MMC probing is intentionally lazy.  Card initialization and archive
  // recovery can block for an unbounded media timeout, so doing them in setup
  // would prevent USB maintenance, BLE, WiFi, and HTTPS from reaching their
  // bounded event loops.  The owned storage probe performs the same mount and
  // archive recovery before any SD test-owned write.
#endif
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
  (void)processing_runtime_.begin(config_manager_.active(), ref,
                                  boot_identity_.sequence, result_context,
                                  profiles_, now_us);
  (void)acquisition_.begin(config_manager_.active(), ref, now_us);
#else
  const uint64_t now_us{0U};
  (void)acquisition_.begin(config_manager_.active(), ref, 0U);
  (void)processing_runtime_.begin(config_manager_.active(), ref,
                                  boot_identity_.sequence, result_context,
                                  profiles_, 0U);
#endif
  if (record_context_.device_id.present) {
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
  }
}

// cppcheck-suppress unusedFunction
void SigapApp::loop() noexcept {
#ifdef ARDUINO
  const acquisition::ArduinoMonotonicClock clock{};
  const uint64_t now_us = clock.now_us();
  provisioning_clock_.set_now(now_us);
  wifi_manager_.poll();
  ble_service_.poll();
  maintenance_service_.poll(now_us);
  std::size_t audit_budget{2U};
  while (audit_budget-- != 0U && archive_runtime_.drain_audit_events(audit_ring_)) {
  }
  refresh_model_profiles();
  if (profile_generation_ != profile_manager_.generation()) {
    profile_generation_ = profile_manager_.generation();
    // Apply profile objects at the channel boundary without rebuilding the
    // scheduler or restarting unrelated sensors.
    (void)acquisition_.refresh_profiles(now_us);
  }
  acquisition_.poll(now_us);
  (void)archive_runtime_.drain_samples(acquisition_.handoff());
  processing_runtime_.poll(now_us);
  (void)archive_runtime_.drain_results(processing_runtime_.handoff());
  // LittleFS and the Arduino VFS share the global descriptor table with the
  // asynchronous HTTPS workers. Keep filesystem ownership in loopTask while
  // either transport task is still active; this prevents a failed TLS socket
  // teardown from colliding with an in-flight filesystem descriptor.
  bool filesystem_transport_busy{false};
  if (network_worker_.has_value() && network_worker_->transport_pending()) {
    filesystem_transport_busy = true;
  }
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  if (device_control_ready_ && device_control_poller_.busy()) {
    filesystem_transport_busy = true;
  }
#endif
  if (!filesystem_transport_busy) {
    (void)archive_runtime_.poll_storage_slice(4U, 32U * 1024U);
    (void)batch_runtime_.poll();
  }
  network_clock_.set_now(now_us);
  bool tls_time_ready{true};
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  const std::time_t wall_clock = std::time(nullptr);
  network_time_ready_ = wall_clock >= static_cast<std::time_t>(1700000000);
  tls_time_ready = network_time_ready_;
#endif
  if (network_ready_ && tls_time_ready && network_worker_.has_value()) {
    (void)network_worker_->poll();
  }
  // The shared controller owns session expiry for every transport, including
  // BLE and USB builds that do not enable the optional HTTPS device-control
  // endpoint.
  calibration_controller_.poll(now_us);
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  if (device_control_ready_ && tls_time_ready) {
    (void)device_control_poller_.poll();
  }
#endif
  (void)archive_runtime_.poll_media_migration();
  maintenance_service_.poll(now_us);
#ifdef ARDUINO
  if (reboot_controller_.consume_request()) {
    ESP.restart();
  }
#endif
#endif
}

}  // namespace sigap
