#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "config/config_manager.h"
#include "sigap/config/config_defaults.h"
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
#ifdef ARDUINO
#include "runtime_debug_observer.h"
#include "utc_source.h"
#endif
#include "models/model_lifecycle.h"
#include "maintenance/model_runtime.h"
#include "maintenance/ble_calibration_backend.h"
#if SIGAP_PROTOTYPE_OPERATION
#include "prototype_commissioning.h"
#include "prototype_live_runtime.h"
#if SIGAP_LIVE_NO_BACKLOG
#include "latest_sample_runtime.h"
#endif
#include "models/prototype_defaults.h"
#endif
#ifdef ARDUINO
#include "debug/http_transport.h"
#include "debug/commands.h"
#include "debug/usb_sink.h"
#include "sigap/build/build_config.h"
#include "device_control_executor.h"
#include "calibration/calibration_replay_store.h"
#include "network/device_control_http.h"
#include "network/device_control_journal.h"
#endif
#include "storage/archive_media.h"
#include "storage/legacy_pending_store.h"
#include "storage/live_archive_store.h"
#include "storage/live_delivery_paths.h"
#include "telemetry_archive/live_batch_runtime.h"
#include <type_traits>
#include "archive_runtime.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/runtime.h"
#include "network/network.h"
#ifdef ARDUINO
#include "network/async_ack_source.h"
#endif
#if defined(ARDUINO) && defined(SIGAP_NETWORK_ENDPOINT) && \
    defined(SIGAP_NETWORK_CA_CERT)
#include "network/https_transport.h"
#endif

namespace sigap {

#if SIGAP_PROTOTYPE_OPERATION
#if SIGAP_LIVE_NO_BACKLOG
using PrototypeDeliveryRuntime = LatestSampleRuntime;
#else
using PrototypeDeliveryRuntime = PrototypeLiveBatchRuntime;
#endif
#endif

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

#ifdef ARDUINO
class ApplicationDebugLock final : public debug::IDebugLock {
 public:
  void lock() noexcept override { portENTER_CRITICAL(&mux_); }
  void unlock() noexcept override { portEXIT_CRITICAL(&mux_); }
 private:
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

// Candidate success persists on a later poll; defer admission during TLS cleanup.
class ApplicationWifiBackend final : public provisioning::IBleProvisioningBackend {
 public:
  ApplicationWifiBackend(provisioning::WifiConnectionManager& manager,
                         debug::RuntimeIoGate& gate) noexcept
      : backend_(manager), gate_(gate) {}
  provisioning::WifiState wifi_state() const noexcept override { return backend_.wifi_state(); }
  std::string_view current_ssid() const noexcept override { return backend_.current_ssid(); }
  provisioning::WifiScanStatus request_scan() noexcept override { return backend_.request_scan(); }
  provisioning::WifiScanStatus poll_scan(provisioning::WifiNetwork* out,
      std::size_t capacity, std::size_t& count) noexcept override {
    return backend_.poll_scan(out, capacity, count);
  }
  provisioning::WifiScanStatus list_known_wifi(provisioning::WifiNetwork* out,
      std::size_t capacity, std::size_t& count) noexcept override {
    return backend_.list_known_wifi(out, capacity, count);
  }
  bool request_connect(std::string_view ssid, std::string_view password) noexcept override {
    return !gate_.active(debug::IoOwner::DebugHttp) && backend_.request_connect(ssid, password);
  }
  bool retry_wifi() noexcept override {
    return !gate_.active(debug::IoOwner::DebugHttp) && backend_.retry_wifi();
  }
  void cancel() noexcept override { backend_.cancel(); }
 private:
  provisioning::WifiProvisioningBackend backend_;
  debug::RuntimeIoGate& gate_;
};

#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
// Polling cleanup must continue after readiness loss, without admitting a new TLS task.
class ApplicationControlTransport final : public network::IDeviceControlTransport {
 public:
  ApplicationControlTransport(network::WiFiDeviceControlHttpTransport& transport,
                              debug::RuntimeIoGate& gate) noexcept
      : transport_(transport), gate_(gate) {}
  void set_time_valid(bool valid) noexcept { time_valid_ = valid; }
  network::TransportResult start_next_command(std::string_view device,
      std::string_view token, uint32_t timeout, network::DeviceControlHttpResponse& out) noexcept override {
    return may_start() ? transport_.start_next_command(device, token, timeout, out)
                       : network::TransportResult::Failed;
  }
  network::TransportResult poll_next_command(network::DeviceControlHttpResponse& out) noexcept override {
    return transport_.poll_next_command(out);
  }
  network::TransportResult start_command_result(std::string_view device,
      std::string_view token, const network::DeviceControlCommand& command,
      const network::DeviceControlResult& result, uint32_t timeout,
      network::DeviceControlHttpResponse& out) noexcept override {
    return may_start() ? transport_.start_command_result(device, token, command, result, timeout, out)
                       : network::TransportResult::Failed;
  }
  network::TransportResult poll_command_result(network::DeviceControlHttpResponse& out) noexcept override {
    return transport_.poll_command_result(out);
  }
  void cancel_command_request() noexcept override { transport_.cancel_command_request(); }
 private:
  bool may_start() const noexcept {
    return time_valid_ && gate_.active(debug::IoOwner::ControlHttp);
  }
  network::WiFiDeviceControlHttpTransport& transport_;
  debug::RuntimeIoGate& gate_;
  bool time_valid_{false};
};
#endif
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

enum class RuntimeDiagnosticEvent : uint8_t {
  SetupComplete,
  LoopAlive,
  FilesystemRecovery,
  ConfigRuntimeReady,
  LittlefsMounted,
  JournalReady,
  NetworkSecretsLoaded,
  NetworkReady,
  WifiConnected,
  SntpTimeReady,
  DeviceControlReady,
  HttpsPollEntered,
  Count,
};

class RuntimeDiagnosticLatch final {
 public:
  bool first(RuntimeDiagnosticEvent event) noexcept {
    const auto index = static_cast<uint8_t>(event);
    if (index >= static_cast<uint8_t>(RuntimeDiagnosticEvent::Count)) {
      return false;
    }
    const uint16_t bit = static_cast<uint16_t>(1U << index);
    if ((seen_ & bit) != 0U) return false;
    seen_ = static_cast<uint16_t>(seen_ | bit);
    return true;
  }

 private:
  uint16_t seen_{0U};
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

#ifdef ARDUINO
class ApplicationDebugUsbWriter final : public debug::IUsbDebugWriter {
 public:
  std::size_t writable() noexcept override;
  std::size_t write(const char* bytes, std::size_t size) noexcept override;
};
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
#ifdef SIGAP_DEBUG_APP_HOST_TEST
  friend struct DebugAppProbe;
#endif
#ifdef ARDUINO
  using BatchStore = telemetry_archive::FsImmutableBatchStore;
  using AckJournal = telemetry_archive::FsAckJournalStore;
  using BatchRuntime = std::conditional_t<build::kLiveArchiveSplit,
      telemetry_archive::LiveBatchRuntime,
      telemetry_archive::ArchiveBatchRuntime<BatchStore, AckJournal>>;
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
#ifdef ARDUINO
  storage::live_delivery::BootPaths live_paths_{boot_identity_.boot_id};
  debug::RuntimeIoGate io_gate_{};
  ApplicationDebugLock debug_lock_{};
  debug::DebugService debug_service_{boot_identity_.boot_id,
      build::kFirmwareBuildId, build::kDebugDefaultLevel, debug_lock_};
  debug::CommandBackend debug_command_backend_{debug_service_};
  ApplicationDebugUsbWriter debug_usb_writer_{};
  debug::UsbDebugSink debug_usb_sink_{debug_service_, debug_usb_writer_};
  std::optional<debug::DebugHttpsTransport> debug_transport_{};
  std::optional<debug::DebugHttpWorker> debug_worker_{};
#endif
#ifdef ARDUINO
  ApplicationUtcSource utc_source_{};
#endif
  acquisition::RecordContext record_context_{};
  acquisition::RecordFactory record_factory_;
  calibration::CalibrationEvidenceCache evidence_cache_{};
#ifdef ARDUINO
  RuntimeDebugObserver runtime_debug_observer_{debug_service_, evidence_cache_};
#endif
  acquisition::TargetProfileSet profiles_{};
#if SIGAP_SENSOR_SIMULATION
  acquisition::Ds18b20Profile simulated_ds_profile_{};
#endif
  acquisition::ProfileAuditRing<16U> audit_ring_{};
  acquisition::NvsProfileStore profile_store_{};
  acquisition::ProfilePersistence profile_persistence_;
  acquisition::ProfileManager profile_manager_;
  acquisition::AcquisitionFoundation acquisition_;
  ProcessingRuntime processing_runtime_{};
#ifdef ARDUINO
  telemetry_archive::BatchJsonBuilder legacy_eligibility_builder_{};
  telemetry_archive::ImmutableBatch legacy_eligibility_scratch_{};
  std::conditional_t<build::kLiveArchiveSplit, storage::LiveArchiveStore,
                     storage::LegacyPendingArchiveStore> littlefs_store_{
      storage::live_delivery::kPriorityFresh ? live_paths_.archive.data() :
          (build::kLiveArchiveSplit ? storage::live_delivery::kArchive : "/sigap/archive.bin"),
      storage::live_delivery::kPriorityFresh ? live_paths_.checkpoint.data() :
          (build::kLiveArchiveSplit ? storage::live_delivery::kCheckpoint : "/sigap/archive.ckp"),
      storage::live_delivery::kPriorityFresh ? live_paths_.gaps.data() :
          (build::kLiveArchiveSplit ? storage::live_delivery::kGaps : "/sigap/gaps.bin")};
  bool legacy_retention_reported_{false};
#else
  storage::LittleFsArchiveStore littlefs_store_{};
#endif
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
  ArchiveBatchNetworkSource<BatchRuntime> network_source_{batch_runtime_};
#if SIGAP_PROTOTYPE_OPERATION
  ArchiveBatchNetworkSource<PrototypeDeliveryRuntime> prototype_network_source_{prototype_batch_runtime_};
#endif
#ifdef ARDUINO
  network::AsyncAckBatchSource async_network_source_{network_source_};
#endif
  ApplicationNetworkClock network_clock_{};
#ifdef ARDUINO
  ApplicationProvisioningClock provisioning_clock_{};
  provisioning::ArduinoWifiStation wifi_station_{};
  provisioning::WifiConnectionManager wifi_manager_;
  ApplicationWifiBackend wifi_backend_;
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
#ifdef ARDUINO
  bool network_time_ready_{false};
#endif
  models::NvsModelProfilePersistence model_persistence_{};
  models::ModelProfileLifecycle model_lifecycle_;
  maintenance::ModelMaintenanceRouter model_router_;
  models::FisModelProfile fis_model_{};
  models::FisModelProfile fis_model_inorganic_{};
  models::FisModelProfile fis_model_b3_{};
  models::SafetyModelProfile safety_model_{};
  bool fis_model_present_{false};
  bool safety_model_present_{false};
#if SIGAP_PROTOTYPE_OPERATION
  // The latest-only mode has no RAM archive or replay queue.
#if !SIGAP_LIVE_NO_BACKLOG
  storage::MemoryArchiveStore<16U> prototype_archive_store_{6ULL * 1024ULL * 1024ULL};
#endif
  // Prototype volatile batch runtime
  PrototypeDeliveryRuntime prototype_batch_runtime_{};
  // Prototype commissioning state
  bool prototype_commissioning_done_{false};
  sigap::CommissioningResult prototype_commissioning_result_{};
  // Prototype model instances (TEST_ONLY, only used when stored models absent)
  models::FisModelProfile prototype_fis_model_{};
  models::SafetyModelProfile prototype_safety_model_{};
  bool prototype_models_built_{false};
#endif
#ifdef ARDUINO
  std::array<uint32_t, 4U> observed_model_versions_{};
  std::array<uint8_t, 4U> observed_model_states_{};
  std::array<bool, 4U> observed_model_valid_{};
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
  calibration::NvsCalibrationReplayStore calibration_replay_store_{};
  ApplicationCalibrationAuthorizer calibration_authorizer_{};
  calibration::CalibrationTransportController calibration_controller_;
  maintenance::CommandRouterBleCalibrationBackend ble_calibration_backend_;
#if defined(SIGAP_NETWORK_ENDPOINT) && defined(SIGAP_NETWORK_CA_CERT)
  ApplicationCalibrationExecutor device_control_executor_;
  network::WiFiDeviceControlHttpTransport device_control_transport_{};
  ApplicationControlTransport control_transport_boundary_{device_control_transport_, io_gate_};
  network::FsDeviceControlResultJournal device_control_result_journal_{};
  network::DeviceControlPoller device_control_poller_;
  bool device_control_ready_{false};
  bool device_control_journal_ready_{false};
  std::array<char, 65U> device_control_id_{};
#endif
#endif
  maintenance::MaintenanceService maintenance_service_;
  uint32_t profile_generation_{0U};
#ifdef ARDUINO
  RuntimeDiagnosticLatch runtime_diagnostics_{};
  uint64_t runtime_diagnostics_started_us_{0U};
  uint64_t previous_loop_entry_us_{0U};
  uint8_t last_loop_gap_class_{255U};
  bool loop_gap_baseline_valid_{false};
  uint64_t last_ack_worker_diagnostic_us_{0U};
  uint64_t last_flow_diagnostic_us_{0U};
  uint64_t last_flow_pending_bytes_{0U};
#if SIGAP_PROTOTYPE_OPERATION
  network::NetworkOutcome last_network_outcome_{network::NetworkOutcome::Idle};
  uint64_t network_poll_count_{0U};
  uint64_t network_delivered_count_{0U};
#endif
  bool samples_first_drain_{true};
#endif

  void refresh_model_profiles() noexcept;
#ifdef ARDUINO
  void log_runtime_diagnostic(RuntimeDiagnosticEvent event, bool ready,
                              uint64_t now_us) noexcept;
  void observe_loop_gap(uint64_t now_us) noexcept;
#endif
};

}  // namespace sigap
