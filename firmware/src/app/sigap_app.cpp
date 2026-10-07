#include "sigap_app.h"

#include "sigap/board/board_memory.h"
#include "sigap/build/build_config.h"
#include "acquisition/provenance.h"
#include "acquisition/arduino_adapters.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>
#ifdef SIGAP_USE_SD
#include <SD.h>
#include <SPI.h>
#endif
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
}  // namespace
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
                       storage::ArchiveMedia::Sd),
      model_lifecycle_(model_persistence_,
                       models::ModelProfilePersistence::default_codec()),
      model_router_(model_lifecycle_,
                    models::ModelProfilePersistence::default_codec()),
      maintenance_router_(config_manager_, secret_store_, &profile_manager_,
                          &evidence_cache_, &reboot_controller_, &audit_ring_,
                          &boot_identity_.boot_id, &model_router_),
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
    (void)littlefs_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes);
    (void)batch_store_.begin(
        LittleFS, config_manager_.active().storage.flash_pending_quota_bytes);
    (void)ack_journal_.begin(LittleFS);
  }
#ifdef SIGAP_USE_SD
  SPI.begin(board::kSdSck, board::kSdMiso, board::kSdMosi, board::kSdCs);
  if (SD.begin(board::kSdCs, SPI, 20'000'000U)) {
    (void)sd_store_.begin(
        SD, config_manager_.active().storage.sd_pending_quota_bytes);
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
  (void)archive_runtime_.poll_storage();
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
