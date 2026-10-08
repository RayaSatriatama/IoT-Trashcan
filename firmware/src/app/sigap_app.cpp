#include "sigap_app.h"

#include <array>
#include <cstdio>
#include <string_view>

#include "sigap/board/board_memory.h"
#include "sigap/board/sd_transport.h"
#include "sigap/build/build_config.h"
#include "acquisition/provenance.h"
#include "acquisition/arduino_adapters.h"

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
bool begin_sd_transport() noexcept {
  const auto& policy = board::kSdTransportPolicy;
#ifdef SIGAP_SD_TRANSPORT_SDMMC_1BIT
  SD_MMC.setPins(policy.clk, policy.cmd_or_miso, policy.d0_or_mosi);
  return SD_MMC.begin(policy.mount_point, policy.mode_1bit,
                      policy.format_if_mount_failed, SDMMC_FREQ_DEFAULT,
                      policy.max_files);
#else
  SPI.begin(policy.clk, policy.cmd_or_miso, policy.d0_or_mosi, policy.cs);
  return SD.begin(policy.cs, SPI, policy.spi_frequency_hz);
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
  constexpr const char* kPath = "/sigap/task6_sd_probe.bin";
  const int64_t started_us = esp_timer_get_time();
  if (!begin_sd_transport()) {
    return maintenance::CommandStatus::Rejected;
  }
  fs::FS& sd = sd_filesystem();
  (void)sd.remove(kPath);
  File output = sd.open(kPath, FILE_WRITE);
  if (!output) return maintenance::CommandStatus::Failed;
  const bool written = output.write(kProbeBytes.data(), kProbeBytes.size()) ==
                       kProbeBytes.size();
  if (written) output.flush();
  const bool flushed = written;
  output.close();
  if (!flushed) {
    (void)sd.remove(kPath);
    return maintenance::CommandStatus::Failed;
  }
  File input = sd.open(kPath, FILE_READ);
  if (!input || input.size() != kProbeBytes.size()) {
    if (input) input.close();
    (void)sd.remove(kPath);
    return maintenance::CommandStatus::Failed;
  }
  std::array<uint8_t, kProbeBytes.size()> readback{};
  const bool read = input.read(readback.data(), readback.size()) ==
                    static_cast<int>(readback.size());
  input.close();
  const auto expected_hash = telemetry_contract::sha256(std::string_view(
      reinterpret_cast<const char*>(kProbeBytes.data()), kProbeBytes.size()));
  const auto actual_hash = telemetry_contract::sha256(std::string_view(
      reinterpret_cast<const char*>(readback.data()), readback.size()));
  const bool hash_match = read && expected_hash == actual_hash &&
                          readback == kProbeBytes;
  const bool removed = sd.remove(kPath);
  if (!hash_match || !removed) return maintenance::CommandStatus::Failed;
  const uint64_t total_bytes = sd_total_bytes();
  const uint64_t used_bytes = sd_used_bytes();
  const uint64_t free_bytes = total_bytes >= used_bytes
                                  ? total_bytes - used_bytes
                                  : 0U;

  char number[32]{};
  const auto append_u64 = [&response, &number](const char* label,
                                                uint64_t value) noexcept {
    const int written = std::snprintf(number, sizeof(number), "%llu",
                                       static_cast<unsigned long long>(value));
    return written > 0 && response.append(label) &&
           response.append(std::string_view(number, static_cast<std::size_t>(written)));
  };
  static constexpr char digits[] = "0123456789abcdef";
  if (!response.append("sd_probe=PASS\n") ||
      !response.append("transport=") ||
      !response.append(board::sd_transport_name(
          board::kSdTransportPolicy.transport)) ||
      !response.append("\n") ||
      !append_u64("bytes=", readback.size()) ||
      !response.append("\n") ||
      !response.append("hash=") ) {
    return maintenance::CommandStatus::Failed;
  }
  for (const uint8_t byte : actual_hash) {
    char pair[2]{digits[byte >> 4U], digits[byte & 0x0FU]};
    if (!response.append(std::string_view(pair, 2U))) {
      return maintenance::CommandStatus::Failed;
    }
  }
  const int64_t ended_us = esp_timer_get_time();
  return response.append("\n") &&
                 append_u64("elapsed_us=", ended_us >= started_us
                                             ? static_cast<uint64_t>(ended_us - started_us)
                                             : 0U) &&
                 response.append("\n") &&
                 append_u64("total_bytes=", total_bytes) &&
                 response.append("\n") &&
                 append_u64("used_bytes=", used_bytes) &&
                 response.append("\n") &&
                 append_u64("free_bytes=", free_bytes) &&
                 response.append("\n")
             ? maintenance::CommandStatus::Accepted
             : maintenance::CommandStatus::Failed;
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
      maintenance_service_(maintenance_transport_, maintenance_router_) {
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
#endif
  const config::ConfigBootResult boot_result = config_manager_.begin();
  (void)boot_result;
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
#ifdef ARDUINO
  if (LittleFS.begin(false)) {
    (void)migration_journal_.begin(LittleFS);
    (void)littlefs_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes);
    (void)batch_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes);
    (void)ack_journal_.begin(LittleFS);
  }
#ifdef SIGAP_USE_SD
  if (begin_sd_transport()) {
    (void)sd_store_.begin(
        sd_filesystem(), config_manager_.active().storage.sd_pending_quota_bytes);
  }
#endif
  (void)archive_store_.recover();
#else
  (void)littlefs_store_.begin(
      config_manager_.active().storage.flash_pending_quota_bytes);
  (void)sd_store_.begin(
      config_manager_.active().storage.sd_pending_quota_bytes);
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
    if (metadata_status == storage::AppendStatus::Queued) {
      (void)archive_runtime_.poll_storage();
    }
  }
}

// cppcheck-suppress unusedFunction
void SigapApp::loop() noexcept {
#ifdef ARDUINO
  const acquisition::ArduinoMonotonicClock clock{};
  const uint64_t now_us = clock.now_us();
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
  (void)archive_runtime_.poll_storage_slice(4U, 32U * 1024U);
  (void)batch_runtime_.poll();
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
