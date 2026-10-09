#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "config/config_manager.h"
#include "config/nvs_config_store.h"
#include "acquisition/boot_identity.h"
#include "acquisition/record.h"
#include "acquisition/runtime.h"
#include "acquisition/profile_manager.h"
#include "acquisition/profile_store.h"
#include "maintenance/runtime.h"
#include "provisioning/secret_store.h"
#include "provisioning/ble_provisioning.h"
#include "calibration/calibration_session.h"
#include "sigap/board/board_pins.h"
#include "processing_runtime.h"
#include "models/model_lifecycle.h"
#include "maintenance/model_runtime.h"
#include "maintenance/ble_calibration_backend.h"
#include "storage/archive_media.h"
#include "archive_runtime.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/runtime.h"
#include "network/network.h"
#if defined(ARDUINO) && defined(SIGAP_NETWORK_ENDPOINT) && \
    defined(SIGAP_NETWORK_CA_CERT)
#include "network/https_transport.h"
#endif

namespace sigap {

// Adapts the durable batch runtime to the network worker without exposing
// mutable archive state. The runtime remains the sole owner of ACK/reclaim
// transitions and the exact immutable body.
template <typename BatchRuntime>
class ArchiveBatchNetworkSource final
    : public network::IImmutableBatchSource {
 public:
  explicit ArchiveBatchNetworkSource(BatchRuntime& runtime) noexcept
      : runtime_(runtime) {}

  bool peek_pending(
      const telemetry_archive::ImmutableBatch*& output) noexcept override {
    if (!runtime_.pending()) {
      output = nullptr;
      return false;
    }
    output = &runtime_.batch();
    return true;
  }

  network::DeliveryStatus acknowledge(
      const telemetry_archive::Ack& ack) noexcept override {
    const auto status = runtime_.apply_ack(ack);
    switch (status) {
      case telemetry_archive::AckPersistStatus::Durable:
        return network::DeliveryStatus::Durable;
      case telemetry_archive::AckPersistStatus::Invalid:
      case telemetry_archive::AckPersistStatus::NotFound:
        return network::DeliveryStatus::Invalid;
      case telemetry_archive::AckPersistStatus::Retryable:
      default:
        return network::DeliveryStatus::Retryable;
    }
  }

 private:
  BatchRuntime& runtime_;
};

class ApplicationNetworkClock final : public network::IMonotonicClock {
 public:
  uint64_t now_us() const noexcept override { return now_us_; }
  void set_now(uint64_t now_us) noexcept { now_us_ = now_us; }

 private:
  uint64_t now_us_{0U};
};

#ifdef ARDUINO
class ApplicationProvisioningClock final
    : public provisioning::IProvisioningClock,
      public provisioning::BleProvisioningClock,
      public maintenance::ICalibrationClock {
 public:
  uint64_t now_us() const noexcept override { return now_us_; }
  void set_now(uint64_t value) noexcept { now_us_ = value; }

 private:
  uint64_t now_us_{0U};
};
#endif

class ApplicationNetworkJitter final : public network::IRandomJitter {
 public:
  uint32_t sample(uint32_t maximum_ms) noexcept override {
    if (maximum_ms == 0U) return 0U;
    // Instance-owned deterministic PRNG keeps retry timing bounded without
    // adding a global RNG or touching the acquisition clock domain.
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    return state_ % (maximum_ms + 1U);
  }

 private:
  uint32_t state_{0x6D2B79F5U};
};

// Network provisioning is intentionally conservative until an endpoint and
// CA bundle are supplied by the build composition. Offline polling remains a
// valid runtime state and never alters acquisition or archive ownership.
class DisabledNetworkLink final : public network::IWifiLink {
 public:
  bool connected() const noexcept override { return false; }
  bool poll_connect() noexcept override { return false; }
};

class DisabledNetworkTransport final : public network::IHttpTransport {
 public:
  network::TransportResult post_batch(
      std::string_view, std::string_view, uint16_t,
      uint32_t, network::NetworkHttpResponse&) noexcept override {
    return network::TransportResult::Failed;
  }
};

#if defined(ARDUINO) && defined(SIGAP_NETWORK_ENDPOINT) && \
    defined(SIGAP_NETWORK_CA_CERT)
class ArduinoNetworkLink final : public network::IWifiLink {
 public:
  void bind(const network::WiFiArchiveHttpTransport* transport) noexcept {
    transport_ = transport;
  }
  bool connected() const noexcept override {
    return transport_ != nullptr && transport_->connected();
  }
  bool poll_connect() noexcept override { return connected(); }

 private:
  const network::WiFiArchiveHttpTransport* transport_{nullptr};
};
#endif

#ifdef ARDUINO
class ArduinoSdProbe final : public maintenance::IStorageTestProbe {
 public:
  void bind_archive_store(storage::SdArchiveStore& store,
                          uint64_t quota_bytes) noexcept {
    archive_store_ = &store;
    archive_quota_bytes_ = quota_bytes;
  }

  maintenance::CommandStatus run_sd_probe(
      maintenance::MaintenanceResponse& response) noexcept override;

 private:
  storage::SdArchiveStore* archive_store_{nullptr};
  uint64_t archive_quota_bytes_{0U};
  storage::ArchiveIdentity archive_identity_{};
  config::ConfigHash archive_hash_{};

};

static_assert(sizeof(ArduinoSdProbe) <= 1024U,
              "SD probe workspace must remain bounded and instance-owned");
#endif

class SigapApp final {
 public:
  SigapApp() noexcept;

  void setup() noexcept;
  void loop() noexcept;

  const config::ConfigManager& config_manager() const noexcept {
    return config_manager_;
  }

  const acquisition::AcquisitionFoundation& acquisition() const noexcept {
    return acquisition_;
  }

  const maintenance::CommandRouter& maintenance_router() const noexcept {
    return maintenance_router_;
  }

  network::NetworkState network_state() const noexcept {
    return network_worker_.has_value() ? network_worker_->state()
                                       : network::NetworkState::Offline;
  }

 private:
#ifdef ARDUINO
  using BatchStore = telemetry_archive::FsImmutableBatchStore;
  using AckJournal = telemetry_archive::FsAckJournalStore;
  using BatchRuntime = telemetry_archive::ArchiveBatchRuntime<
      BatchStore, AckJournal>;
#else
  using BatchStore = telemetry_archive::MemoryImmutableBatchStore<8U>;
  using AckJournal = telemetry_archive::MemoryAckJournalStore<8U>;
  using BatchRuntime = telemetry_archive::ArchiveBatchRuntime<
      BatchStore, AckJournal>;
#endif
  config::NvsConfigStore config_store_{};
  config::ConfigManager config_manager_;
  provisioning::NvsSecretStore secret_store_{};
  acquisition::BootRuntimeIdentity boot_identity_{};
  acquisition::RecordContext record_context_{};
  acquisition::RecordFactory record_factory_;
  calibration::CalibrationEvidenceCache evidence_cache_{};
  acquisition::TargetProfileSet profiles_{};
  acquisition::ProfileAuditRing<16U> audit_ring_{};
  acquisition::NvsProfileStore profile_store_{};
  acquisition::ProfilePersistence profile_persistence_;
  acquisition::ProfileManager profile_manager_;
  acquisition::AcquisitionFoundation acquisition_;
  ProcessingRuntime processing_runtime_{};
  storage::LittleFsArchiveStore littlefs_store_{};
  storage::SdArchiveStore sd_store_{};
  storage::MediaFailoverStore archive_store_;
  storage::ArchiveStoreMigrationSource migration_source_;
  storage::ArchiveStoreMigrationDestination migration_destination_;
#ifdef ARDUINO
  storage::FsMigrationReceiptJournal migration_journal_{};
#endif
  storage::ArchiveMediaMigration media_migration_;
  ArchiveRuntime archive_runtime_{};
#ifdef ARDUINO
  BatchStore batch_store_{};
#else
  BatchStore batch_store_{6ULL * 1024ULL * 1024ULL};
#endif
#ifdef ARDUINO
  AckJournal ack_journal_{};
#else
  AckJournal ack_journal_{};
#endif
  BatchRuntime batch_runtime_{};
  ArchiveBatchNetworkSource<BatchRuntime> network_source_;
  ApplicationNetworkClock network_clock_{};
#ifdef ARDUINO
  ApplicationProvisioningClock provisioning_clock_{};
  provisioning::ArduinoWifiStation wifi_station_{};
  provisioning::WifiConnectionManager wifi_manager_;
  provisioning::WifiProvisioningBackend wifi_backend_;
  provisioning::BleProvisioningSession ble_session_;
  provisioning::ArduinoBleProvisioningService ble_service_{};
#endif
  ApplicationNetworkJitter network_jitter_{};
#if defined(ARDUINO) && defined(SIGAP_NETWORK_ENDPOINT) && \
    defined(SIGAP_NETWORK_CA_CERT)
  ArduinoNetworkLink network_link_{};
  network::WiFiArchiveHttpTransport network_transport_{};
#else
  DisabledNetworkLink network_link_{};
  DisabledNetworkTransport network_transport_{};
#endif
  std::optional<network::NetworkWorker> network_worker_{};
  bool network_ready_{false};
  models::NvsModelProfilePersistence model_persistence_{};
  models::ModelProfileLifecycle model_lifecycle_;
  maintenance::ModelMaintenanceRouter model_router_;
  models::FisModelProfile fis_model_{};
  models::FisModelProfile fis_model_inorganic_{};
  models::FisModelProfile fis_model_b3_{};
  models::SafetyModelProfile safety_model_{};
  bool fis_model_present_{false};
  bool safety_model_present_{false};
#ifdef ARDUINO
  maintenance::ArduinoUsbTransport maintenance_transport_{};
  maintenance::ArduinoRebootController reboot_controller_{};
#else
  maintenance::NullTransport maintenance_transport_{};
  maintenance::NullRebootController reboot_controller_{};
#endif
#ifdef ARDUINO
  ArduinoSdProbe sd_probe_{};
#endif
  maintenance::CommandRouter maintenance_router_;
#ifdef ARDUINO
  maintenance::CommandRouterBleCalibrationBackend ble_calibration_backend_;
#endif
  maintenance::MaintenanceService maintenance_service_;
  uint32_t profile_generation_{0U};

  void refresh_model_profiles() noexcept;
};

}  // namespace sigap
