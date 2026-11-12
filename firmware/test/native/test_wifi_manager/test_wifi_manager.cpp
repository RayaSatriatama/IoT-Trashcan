#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <cstring>

#include "provisioning/secret_store.h"
#include "provisioning/wifi_manager.h"
#include "provisioning/ble_provisioning.h"
#include "debug/service.h"
#include "debug/registry.h"
#include "debug/types.h"
#include "unity.h"

using namespace sigap::provisioning;

namespace {
class Sink final : public sigap::debug::IEventSink {
 public:
  bool emit(const sigap::debug::EventInput& event) noexcept override {
    ++attempts;
    last = event;
    if (!accept || size == events.size()) return false;
    events[size++] = event;
    return true;
  }
  bool accept{true};
  std::size_t attempts{0U};
  sigap::debug::EventInput last{};
  std::array<sigap::debug::EventInput, 128U> events{};
  std::size_t size{0U};
};

class Clock final : public IProvisioningClock {
 public:
  uint64_t now_us() const noexcept override { return now; }
  uint64_t now{0U};
};

class Station final : public IWifiStation {
 public:
  bool begin_candidate(std::string_view ssid,
                       std::string_view password) noexcept override {
    begin_calls++;
    requested_ssid.assign(ssid);
    requested_password.assign(password);
    return begin_ok;
  }
  bool connected() const noexcept override { return online; }
  bool has_ip() const noexcept override { return online && ip; }
  bool failed() const noexcept override { return failure; }
  void disconnect() noexcept override { disconnect_calls++; online = false; }
  bool begin_scan() noexcept override { ++scan_begin_calls; scanning = scan_begin_ok; return scan_begin_ok; }
  ScanFailureObservation scan_failure_observation() const noexcept override { return scan_failure; }
  WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                           std::size_t& count) noexcept override {
    ++scan_poll_calls;
    if (!scanning) return WifiScanStatus::Idle;
    if (scan_result != WifiScanStatus::Pending) scanning = false;
    if (output != nullptr && capacity != 0U && scan_result == WifiScanStatus::Ready) {
      std::strcpy(output[0].ssid.data(), "AP");
      count = scan_count;
    }
    return scan_result;
  }
  bool begin_ok{true};
  bool online{false};
  bool ip{true};
  bool failure{false};
  bool scanning{false};
  bool scan_begin_ok{true};
  WifiScanStatus scan_result{WifiScanStatus::Ready};
  ScanFailureObservation scan_failure{ScanFailureObservation::Unknown};
  std::size_t scan_count{1U};
  int scan_begin_calls{0};
  int scan_poll_calls{0};
  int begin_calls{0};
  int disconnect_calls{0};
  std::string requested_ssid;
  std::string requested_password;
};
}  // namespace

class TrackingBlobStore final : public ISecretBlobStore {
 public:
  SecretBlobReadStatus read(SecretSlot slot, uint8_t* output, std::size_t capacity,
                            std::size_t& size) const noexcept override {
    if (slot == SecretSlot::Wifi) ++wifi_reads;
    else ++token_reads;
    return store.read(slot, output, capacity, size);
  }
  bool write(SecretSlot slot, const uint8_t* bytes, std::size_t size) noexcept override {
    if (slot == SecretSlot::Wifi) ++wifi_writes;
    else ++token_writes;
    return store.write(slot, bytes, size);
  }
  bool clear_all() noexcept override { return store.clear_all(); }
  mutable int wifi_reads{0};
  mutable int token_reads{0};
  int wifi_writes{0};
  int token_writes{0};
  MemorySecretBlobStore store;
};

void test_saved_wifi_load_ignores_absent_or_corrupt_api_token() {
  for (const bool corrupt_token : {false, true}) {
    TrackingBlobStore backend;
    PersistentSecretStore secrets(backend);
    TEST_ASSERT_TRUE(secrets.set_wifi("SAVED", "saved-password"));
    if (corrupt_token) {
      TEST_ASSERT_TRUE(secrets.rotate_device_token("test-token"));
      backend.store.corrupt(SecretSlot::DeviceToken);
    }
    backend.wifi_reads = 0;
    backend.token_reads = 0;
    backend.wifi_writes = 0;
    backend.token_writes = 0;
    Clock clock;
    Station station;
    WifiConnectionManager manager(secrets, station, clock);
    TEST_ASSERT_TRUE(manager.begin());
    TEST_ASSERT_EQUAL_INT(1, backend.wifi_reads);
    TEST_ASSERT_EQUAL_INT(0, backend.token_reads);
    TEST_ASSERT_EQUAL_INT(0, backend.wifi_writes);
    TEST_ASSERT_EQUAL_INT(0, backend.token_writes);
    TEST_ASSERT_EQUAL_STRING("SAVED", station.requested_ssid.c_str());
    NetworkSecrets strict{};
    TEST_ASSERT_FALSE(secrets.load_for_network(strict));
    TEST_ASSERT_EQUAL_INT(1, backend.token_reads);
    TEST_ASSERT_EQUAL_STRING("", strict.ssid.data());
  }
}

void test_wifi_startup_uses_wifi_only_load_without_token_access() {
  class WifiOnlySecrets final : public SecretStore {
   public:
    bool load_for_network(NetworkSecrets&) override {
      ++combined_loads;
      return false;
    }
    bool load_wifi_for_network(NetworkSecrets& output) override {
      ++wifi_loads;
      std::strcpy(output.ssid.data(), "SAVED");
      std::strcpy(output.password.data(), "saved-password");
      return true;
    }
    SecretPresence status() const noexcept override { return {}; }
    bool set_wifi(const char*, const char*) override { return false; }
    bool rotate_device_token(const char*) override { ++token_writes; return false; }
    bool clear_all(bool) override { return false; }
    SecretStoreResult last_result() const noexcept override { return SecretStoreResult::Ok; }
    int combined_loads{0};
    int wifi_loads{0};
    int token_writes{0};
  } secrets;
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  TEST_ASSERT_TRUE(manager.begin());
  TEST_ASSERT_EQUAL_INT(1, secrets.wifi_loads);
  TEST_ASSERT_EQUAL_INT(0, secrets.combined_loads);
  TEST_ASSERT_EQUAL_INT(0, secrets.token_writes);
  TEST_ASSERT_EQUAL_STRING("SAVED", station.requested_ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("saved-password", station.requested_password.c_str());
}

void test_candidate_failure_preserves_saved_credentials() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  TEST_ASSERT_TRUE(secrets.set_wifi("OLD", "old-password"));
  TEST_ASSERT_TRUE(secrets.rotate_device_token("device-token"));
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  TEST_ASSERT_TRUE(manager.begin());
  TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
  station.online = true;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
  station.failure = true;
  TEST_ASSERT_TRUE(manager.request_candidate("NEW", "new-password"));
  manager.poll();
  NetworkSecrets loaded{};
  TEST_ASSERT_TRUE(secrets.load_for_network(loaded));
  TEST_ASSERT_EQUAL_STRING("OLD", loaded.ssid.data());
  TEST_ASSERT_EQUAL_STRING("old-password", loaded.password.data());
}

void test_successful_candidate_persists_after_ip() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  TEST_ASSERT_TRUE(secrets.set_wifi("OLD", "old-password"));
  TEST_ASSERT_TRUE(secrets.rotate_device_token("device-token"));
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  TEST_ASSERT_TRUE(manager.begin());
  station.online = true;
  manager.poll();
  TEST_ASSERT_TRUE(manager.request_candidate("NEW", "new-password"));
  station.online = true;
  manager.poll();
  NetworkSecrets loaded{};
  TEST_ASSERT_TRUE(secrets.load_for_network(loaded));
  TEST_ASSERT_EQUAL_STRING("NEW", loaded.ssid.data());
  TEST_ASSERT_EQUAL_STRING("new-password", loaded.password.data());
}

void test_scan_is_rate_limited() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, manager.request_scan());
  std::array<WifiNetwork, 2> networks{};
  std::size_t count{0U};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
                        manager.poll_scan(networks.data(), networks.size(), count));
  TEST_ASSERT_EQUAL_UINT(1U, count);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.request_scan());
}

void test_empty_known_network_list_is_ready_before_first_provisioning() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  TEST_ASSERT_TRUE(manager.begin());
  std::array<WifiNetwork, 2> networks{};
  std::size_t count{99U};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
                        manager.list_known(networks.data(), networks.size(), count));
  TEST_ASSERT_EQUAL_UINT(0U, count);
}

void test_wifi_scan_events_report_observed_lifecycle() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Operator));
  TEST_ASSERT_EQUAL_UINT(1U, sink.size);
  TEST_ASSERT_EQUAL_UINT(20U, sink.events[0].event_code);
  TEST_ASSERT_EQUAL_INT(0, sink.events[0].arg0);
  TEST_ASSERT_EQUAL_INT(1, sink.events[0].arg1);
  clock.now = 1500000U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, manager.request_scan());
  TEST_ASSERT_EQUAL_UINT(2U, sink.size);
  TEST_ASSERT_EQUAL_UINT(21U, sink.events[1].event_code);
  TEST_ASSERT_EQUAL_INT(1500000, sink.events[1].arg1);
  clock.now = 2500000U;
  std::array<WifiNetwork, 2> networks{};
  std::size_t count{};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
      manager.poll_scan(networks.data(), networks.size(), count));
  TEST_ASSERT_EQUAL_UINT(3U, sink.size);
  TEST_ASSERT_EQUAL_UINT(22U, sink.events[2].event_code);
  TEST_ASSERT_EQUAL_INT(1, sink.events[2].arg0);
  TEST_ASSERT_EQUAL_INT(2500000, sink.events[2].arg1);
  clock.now = 14999000U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.request_scan());
  TEST_ASSERT_EQUAL_UINT(4U, sink.size);
  TEST_ASSERT_EQUAL_UINT(23U, sink.events[3].event_code);
  TEST_ASSERT_EQUAL_INT(1, sink.events[3].arg0);
}

void test_wifi_scan_unknown_failure_does_not_invent_reason() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  station.scan_begin_ok = false;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Failed, manager.request_scan());
  TEST_ASSERT_EQUAL_UINT(0U, sink.size);
  station.scan_begin_ok = true;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Internal));
  station.scan_failure = ScanFailureObservation::Driver;
  station.scan_result = WifiScanStatus::Failed;
  std::size_t count{};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Failed, manager.poll_scan(nullptr, 0U, count));
  TEST_ASSERT_EQUAL_UINT(2U, sink.size);
  TEST_ASSERT_EQUAL_UINT(24U, sink.events[1].event_code);
  TEST_ASSERT_EQUAL_INT(0, sink.events[1].arg0);
}

void test_wifi_link_retry_cycles_emit_only_real_state_changes() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  TEST_ASSERT_TRUE(secrets.set_wifi("SAVED", "[REDACTED]"));
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_TRUE(manager.begin());
  TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
  station.online = true;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
  const auto after_up = sink.size;
  manager.poll();
  TEST_ASSERT_EQUAL_UINT(after_up, sink.size);
  station.online = false;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::Backoff, manager.state());
  TEST_ASSERT_EQUAL_INT(1, sink.events[sink.size - 1U].arg1);
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
  TEST_ASSERT_EQUAL_INT(1, sink.events[sink.size - 1U].arg1);
  station.online = true;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
  TEST_ASSERT_EQUAL_INT(2, sink.events[sink.size - 1U].arg0);
}

void test_pending_scan_polling_is_quiet_and_zero_ready_is_reported() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  station.scan_result = WifiScanStatus::Pending;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Operator));
  TEST_ASSERT_EQUAL_UINT(1U, sink.size);
  std::array<WifiNetwork, 2> networks{};
  std::size_t count{99U};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.poll_scan(networks.data(), networks.size(), count));
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.poll_scan(networks.data(), networks.size(), count));
  TEST_ASSERT_EQUAL_UINT(0U, count);
  TEST_ASSERT_EQUAL_UINT(1U, sink.size);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, manager.request_scan());
  TEST_ASSERT_EQUAL_UINT(2U, sink.size);
  TEST_ASSERT_EQUAL_UINT(21U, sink.events[1].event_code);
  station.scan_result = WifiScanStatus::Ready;
  station.scan_count = 0U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
      manager.poll_scan(networks.data(), networks.size(), count));
  TEST_ASSERT_EQUAL_UINT(0U, count);
  TEST_ASSERT_EQUAL_UINT(3U, sink.size);
  TEST_ASSERT_EQUAL_UINT(22U, sink.events[2].event_code);
  TEST_ASSERT_EQUAL_INT(0, sink.events[2].arg0);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Idle,
      manager.poll_scan(networks.data(), networks.size(), count));
  TEST_ASSERT_EQUAL_UINT(3U, sink.size);
}

void test_candidate_credentials_write_only_after_ip_and_only_once() {
  TrackingBlobStore backend;
  PersistentSecretStore secrets(backend);
  TEST_ASSERT_TRUE(secrets.set_wifi("OLD", "[REDACTED]"));
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  TEST_ASSERT_TRUE(manager.begin());
  backend.wifi_writes = 0;
  TEST_ASSERT_TRUE(manager.request_candidate("NEW", "[REDACTED]"));
  station.online = true;
  station.ip = false;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(0, backend.wifi_writes);
  TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
  station.ip = true;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(1, backend.wifi_writes);
  TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
  manager.poll();
  TEST_ASSERT_EQUAL_INT(1, backend.wifi_writes);
  NetworkSecrets saved{};
  TEST_ASSERT_TRUE(secrets.load_wifi_for_network(saved));
  TEST_ASSERT_EQUAL_STRING("NEW", saved.ssid.data());
}

void test_operator_cancel_uses_normal_diagnostic_reason() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  TEST_ASSERT_TRUE(secrets.set_wifi("SAVED", "[REDACTED]"));
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_TRUE(manager.begin());
  TEST_ASSERT_TRUE(manager.request_candidate("CANDIDATE", "[REDACTED]"));
  manager.cancel();
  TEST_ASSERT_EQUAL_INT(WifiState::Backoff, manager.state());
  TEST_ASSERT_EQUAL_UINT(25U, sink.events[sink.size - 1U].event_code);
  TEST_ASSERT_EQUAL_INT(0, sink.events[sink.size - 1U].arg0);
  TEST_ASSERT_EQUAL_INT(0, sink.events[sink.size - 1U].arg1);
}

void test_wifi_write_fault_preserves_saved_credentials_and_never_reports_up() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  TEST_ASSERT_TRUE(secrets.set_wifi("OLD", "[REDACTED]"));
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_TRUE(manager.begin());
  station.online = true;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
  TEST_ASSERT_TRUE(manager.request_candidate("NEW", "[REDACTED]"));
  station.online = true;
  backend.fail_next(MemorySecretBlobStore::Fault::WriteWifi);
  const auto prior_events = sink.size;
  manager.poll();
  TEST_ASSERT_NOT_EQUAL(WifiState::Connected, manager.state());
  NetworkSecrets saved{};
  TEST_ASSERT_TRUE(secrets.load_wifi_for_network(saved));
  TEST_ASSERT_EQUAL_STRING("OLD", saved.ssid.data());
  for (std::size_t i = prior_events; i < sink.size; ++i) {
    if (sink.events[i].event_code == 25U)
      TEST_ASSERT_NOT_EQUAL(2, sink.events[i].arg0);
  }
}

namespace {
void assert_wifi_input(const sigap::debug::EventInput& event, uint16_t code,
                       int32_t arg0, int32_t arg1, uint64_t at) {
  TEST_ASSERT_EQUAL_INT(sigap::debug::Module::Wifi, event.module);
  TEST_ASSERT_EQUAL_INT(sigap::debug::Level::Info, event.level);
  TEST_ASSERT_TRUE(sigap::debug::registered(event.module, event.event_code));
  TEST_ASSERT_EQUAL_UINT(code, event.event_code);
  TEST_ASSERT_EQUAL_INT(arg0, event.arg0);
  TEST_ASSERT_EQUAL_INT(arg1, event.arg1);
  TEST_ASSERT_EQUAL_UINT64(at, event.monotonic_us);
  TEST_ASSERT_EQUAL_UINT(0U, event.presence);
  TEST_ASSERT_EQUAL_UINT(0U, event.request_id);
  TEST_ASSERT_EQUAL_UINT64(0U, event.related_sequence);
  TEST_ASSERT_EQUAL_UINT(0U, event.duration_us);
}

EncodedSecretBlob wifi_bytes(const ISecretBlobStore& backend) {
  EncodedSecretBlob result{};
  TEST_ASSERT_EQUAL_INT(SecretBlobReadStatus::Found,
      backend.read(SecretSlot::Wifi, result.bytes.data(), result.bytes.size(), result.size));
  return result;
}

class DebugLock final : public sigap::debug::IDebugLock {
 public:
  void lock() noexcept override { ++entries; }
  void unlock() noexcept override {}
  unsigned entries{0U};
};

class ForwardingSink final : public sigap::debug::IEventSink {
 public:
  explicit ForwardingSink(sigap::debug::IEventSink& target) : target_(target) {}
  bool emit(const sigap::debug::EventInput& event) noexcept override {
    ++attempts;
    last = event;
    const bool result = target_.emit(event);
    if (result) ++accepted;
    return result;
  }
  std::size_t attempts{0U};
  std::size_t accepted{0U};
  sigap::debug::EventInput last{};
 private:
  sigap::debug::IEventSink& target_;
};
}  // namespace

void test_scan_origins_and_attempts_follow_real_admission() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  ProvisioningPolicy policy;
  policy.scan_cooldown_ms = 1U;
  WifiConnectionManager manager(secrets, station, clock, policy);
  WifiProvisioningBackend ble_backend(manager);
  Sink sink;
  manager.bind_debug_sink(&sink);
  station.scan_begin_ok = false;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Failed, ble_backend.request_scan());
  TEST_ASSERT_EQUAL_UINT(0U, sink.size);
  station.scan_begin_ok = true;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, manager.request_scan());
  TEST_ASSERT_EQUAL_UINT(0U, sink.size);
  std::array<WifiNetwork, 2> rows{};
  std::size_t count{};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
      manager.poll_scan(rows.data(), rows.size(), count));
  TEST_ASSERT_EQUAL_UINT(1U, count);
  const auto calls = station.scan_begin_calls;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, ble_backend.request_scan());
  TEST_ASSERT_EQUAL_INT(calls, station.scan_begin_calls);
  clock.now += static_cast<uint64_t>(policy.scan_cooldown_ms) * 1000U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, ble_backend.request_scan());
  assert_wifi_input(sink.last, 20U, 0, 2, clock.now);
  const auto accepted_calls = station.scan_begin_calls;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Internal));
  TEST_ASSERT_EQUAL_INT(accepted_calls, station.scan_begin_calls);
  TEST_ASSERT_EQUAL_UINT(21U, sink.last.event_code);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
      manager.poll_scan(rows.data(), rows.size(), count));
  clock.now += static_cast<uint64_t>(policy.scan_cooldown_ms) * 1000U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Internal));
  assert_wifi_input(sink.last, 20U, 1, 3, clock.now);
  TEST_ASSERT_EQUAL_INT(accepted_calls + 1, station.scan_begin_calls);
}

void test_scan_clock_boundaries_and_saturated_observed_duration() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  clock.now = 1000U;
  Station station;
  ProvisioningPolicy policy;
  policy.scan_cooldown_ms = 1501U;
  WifiConnectionManager manager(secrets, station, clock, policy);
  Sink sink;
  manager.bind_debug_sink(&sink);
  const auto start = clock.now;
  const uint64_t cooldown = static_cast<uint64_t>(policy.scan_cooldown_ms) * 1000U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Operator));
  std::array<WifiNetwork, 2> rows{};
  std::size_t count{};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
      manager.poll_scan(rows.data(), rows.size(), count));
  clock.now = start - 1U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.request_scan());
  const auto full_seconds = static_cast<int32_t>(cooldown / 1000000U +
                                                (cooldown % 1000000U != 0U));
  assert_wifi_input(sink.last, 23U, full_seconds, 0, clock.now);
  clock.now = start + cooldown - 100001U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.request_scan());
  assert_wifi_input(sink.last, 23U, 1, 0, clock.now);
  clock.now = start + cooldown - 1U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.request_scan());
  TEST_ASSERT_EQUAL_INT(1, sink.last.arg0);
  const auto before_expiry = station.scan_begin_calls;
  clock.now = start + cooldown;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Internal));
  TEST_ASSERT_EQUAL_INT(before_expiry + 1, station.scan_begin_calls);
  assert_wifi_input(sink.last, 20U, 1, 2, clock.now);
  const auto second_start = clock.now;
  clock.now = second_start - 1U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, manager.request_scan());
  assert_wifi_input(sink.last, 21U, 0, 0, clock.now);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
      manager.poll_scan(rows.data(), rows.size(), count));
  assert_wifi_input(sink.last, 22U, 1, 0, clock.now);
  clock.now = second_start + cooldown;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Operator));
  clock.now += static_cast<uint64_t>(INT32_MAX) + 100U;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, manager.request_scan());
  assert_wifi_input(sink.last, 21U, 0, INT32_MAX, clock.now);
  station.scan_result = WifiScanStatus::Pending;
  const auto quiet = sink.attempts;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.poll_scan(rows.data(), rows.size(), count));
  TEST_ASSERT_EQUAL_UINT(quiet, sink.attempts);
  station.scan_result = WifiScanStatus::Ready;
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
      manager.poll_scan(rows.data(), rows.size(), count));
  assert_wifi_input(sink.last, 22U, 1, INT32_MAX, clock.now);
}

void test_large_scan_policy_uses_wide_retry_arithmetic() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  ProvisioningPolicy policy;
  policy.scan_cooldown_ms = std::numeric_limits<uint32_t>::max();
  WifiConnectionManager manager(secrets, station, clock, policy);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Operator));
  std::size_t count{};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready, manager.poll_scan(nullptr, 0U, count));
  TEST_ASSERT_EQUAL_UINT(0U, count);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.request_scan());
  const uint64_t us = static_cast<uint64_t>(policy.scan_cooldown_ms) * 1000U;
  const auto seconds = static_cast<int32_t>(us / 1000000U + (us % 1000000U != 0U));
  assert_wifi_input(sink.last, 23U, seconds, 0, clock.now);
}

void test_scan_failure_provenance_is_not_inferred_or_carried_into_other_statuses() {
  for (const bool start_failure : {false, true}) {
    for (const auto failure : {ScanFailureObservation::Unknown, ScanFailureObservation::Driver}) {
      MemorySecretBlobStore backend;
      PersistentSecretStore secrets(backend);
      Clock clock;
      Station station;
      WifiConnectionManager manager(secrets, station, clock);
      Sink sink;
      manager.bind_debug_sink(&sink);
      station.scan_failure = failure;
      TEST_ASSERT_EQUAL_INT(ScanFailureObservation::Unknown,
          station.IWifiStation::scan_failure_observation());
      std::size_t count{};
      if (!start_failure) {
        TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
            manager.request_scan(WifiScanSource::Operator));
        station.scan_result = WifiScanStatus::Failed;
      } else {
        station.scan_begin_ok = false;
      }
      const auto before = sink.attempts;
      const auto status = start_failure ? manager.request_scan(WifiScanSource::Operator) :
                                         manager.poll_scan(nullptr, 0U, count);
      TEST_ASSERT_EQUAL_INT(WifiScanStatus::Failed, status);
      const auto emitted = failure == ScanFailureObservation::Driver ? 1U : 0U;
      TEST_ASSERT_EQUAL_UINT(before + emitted, sink.attempts);
      if (emitted != 0U) assert_wifi_input(sink.last, 24U, 0, 0, clock.now);
      if (start_failure) {
        station.scan_begin_ok = true;
        TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
            manager.request_scan(WifiScanSource::Internal));
        assert_wifi_input(sink.last, 20U, 1, 1, clock.now);
      } else {
        const auto after_failure = sink.attempts;
        TEST_ASSERT_EQUAL_INT(WifiScanStatus::Idle, manager.poll_scan(nullptr, 0U, count));
        TEST_ASSERT_EQUAL_UINT(after_failure, sink.attempts);
      }
    }
  }
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
      manager.request_scan(WifiScanSource::Operator));
  station.scan_failure = ScanFailureObservation::Driver;
  station.scan_result = WifiScanStatus::RateLimited;
  const auto before = sink.attempts;
  std::size_t count{99U};
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.poll_scan(nullptr, 0U, count));
  TEST_ASSERT_EQUAL_UINT(0U, count);
  TEST_ASSERT_EQUAL_UINT(before, sink.attempts);
  TEST_ASSERT_EQUAL_INT(WifiScanStatus::Idle, manager.poll_scan(nullptr, 0U, count));
}

void test_two_association_failure_deadline_retry_up_cycles_preserve_saved_bytes() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  const std::string saved_ssid(8U, 'A'), saved_password(12U, 'a');
  TEST_ASSERT_TRUE(secrets.set_wifi(saved_ssid.c_str(), saved_password.c_str()));
  const auto original = wifi_bytes(backend);
  Clock clock;
  Station station;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_TRUE(manager.begin());
  station.online = true;
  manager.poll();
  const std::string candidate_ssid(8U, 'B'), candidate_password(12U, 'b');
  for (unsigned cycle = 0U; cycle < 2U; ++cycle) {
    TEST_ASSERT_TRUE(manager.request_candidate(candidate_ssid.c_str(), candidate_password.c_str()));
    station.failure = true;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(WifiState::Backoff, manager.state());
    assert_wifi_input(sink.last, 25U, 0, 2, clock.now);
    const auto calls = station.begin_calls;
    const auto events = sink.attempts;
    const auto deadline = clock.now + 1000000U;
    clock.now = deadline - 1U;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(calls, station.begin_calls);
    TEST_ASSERT_EQUAL_UINT(events, sink.attempts);
    clock.now = deadline;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(calls + 1, station.begin_calls);
    TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
    assert_wifi_input(sink.last, 25U, 1, 1, clock.now);
    station.failure = false;
    station.online = true;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
    assert_wifi_input(sink.last, 25U, 2, 0, clock.now);
    const auto after_up = sink.attempts;
    manager.poll();
    TEST_ASSERT_EQUAL_UINT(after_up, sink.attempts);
    const auto stored = wifi_bytes(backend);
    TEST_ASSERT_EQUAL_UINT(original.size, stored.size);
    TEST_ASSERT_TRUE(original.bytes == stored.bytes);
  }
}

void test_exact_down_states_and_association_timeout_are_observable_without_duplicate_polls() {
  MemorySecretBlobStore backend;
  PersistentSecretStore secrets(backend);
  const std::string ssid(8U, 'C'), password(12U, 'c');
  TEST_ASSERT_TRUE(secrets.set_wifi(ssid.c_str(), password.c_str()));
  Clock clock;
  Station station;
  station.begin_ok = false;
  WifiConnectionManager manager(secrets, station, clock);
  Sink sink;
  manager.bind_debug_sink(&sink);
  TEST_ASSERT_TRUE(manager.begin());
  assert_wifi_input(sink.last, 25U, 0, 2, clock.now);
  TEST_ASSERT_TRUE(secrets.clear_all(true));
  TEST_ASSERT_TRUE(manager.begin());
  TEST_ASSERT_EQUAL_UINT(2U, sink.attempts);
  assert_wifi_input(sink.last, 25U, 0, 0, clock.now);
  TEST_ASSERT_TRUE(manager.begin());
  manager.cancel();
  TEST_ASSERT_EQUAL_UINT(2U, sink.attempts);
  station.begin_ok = true;
  TEST_ASSERT_TRUE(manager.request_candidate(ssid.c_str(), password.c_str()));
  const auto attempts = sink.attempts;
  clock.now = static_cast<uint64_t>(kDefaultProvisioningPolicy.wifi_connect_timeout_ms) * 1000U - 1U;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
  TEST_ASSERT_EQUAL_UINT(attempts, sink.attempts);
  ++clock.now;
  manager.poll();
  TEST_ASSERT_EQUAL_INT(WifiState::SetupRequired, manager.state());
  assert_wifi_input(sink.last, 25U, 0, 2, clock.now);
  manager.poll();
  TEST_ASSERT_EQUAL_UINT(attempts + 1U, sink.attempts);
}

void test_sink_backpressure_cannot_change_scan_link_or_persistence_business_effects() {
  // Fresh lifecycle per mode: absent, recording, refusing, filtered, real full ring.
  for (unsigned mode = 0U; mode < 5U; ++mode) {
    TrackingBlobStore backend;
    PersistentSecretStore secrets(backend);
    const std::string old_ssid(8U, 'D'), old_password(12U, 'd');
    const std::string new_ssid(8U, 'E'), new_password(12U, 'e');
    const std::string bad_ssid(8U, 'F'), bad_password(12U, 'f');
    TEST_ASSERT_TRUE(secrets.set_wifi(old_ssid.c_str(), old_password.c_str()));
    backend.wifi_writes = 0;
    Clock clock;
    clock.now = 1U;
    Station station;
    WifiConnectionManager manager(secrets, station, clock);
    Sink sink;
    sink.accept = mode != 2U;
    sigap::acquisition::BootId boot{};
    boot.present = true;
    boot.bytes[0] = 1U;
    DebugLock lock;
    sigap::debug::DebugService service(boot, "wifi-native", sigap::debug::Level::Info, lock);
    ForwardingSink forwarding(service);
    if (mode == 3U || mode == 4U) {
      sigap::debug::EventInput event{};
      event.event_code = 1U;
      event.level = sigap::debug::Level::Error;
      const auto fill = mode == 4U ? 64U : 1U;
      for (unsigned i = 0U; i < fill; ++i) TEST_ASSERT_TRUE(service.emit(event));
      if (mode == 3U) TEST_ASSERT_TRUE(service.set_level(sigap::debug::Level::Error));
      manager.bind_debug_sink(&forwarding);
    } else if (mode != 0U) {
      manager.bind_debug_sink(&sink);
    }
    const auto meta_before = service.meta(clock.now);
    TEST_ASSERT_TRUE(manager.begin());
    TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
    station.online = true;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
    TEST_ASSERT_EQUAL_INT(0, backend.wifi_writes);
    TEST_ASSERT_TRUE(manager.request_candidate(new_ssid.c_str(), new_password.c_str()));
    manager.poll();
    TEST_ASSERT_EQUAL_INT(0, backend.wifi_writes);
    station.online = true;
    station.ip = false;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(0, backend.wifi_writes);
    TEST_ASSERT_EQUAL_INT(WifiState::Connecting, manager.state());
    station.ip = true;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(1, backend.wifi_writes);
    TEST_ASSERT_EQUAL_INT(WifiState::Connected, manager.state());
    const auto accepted_blob = wifi_bytes(backend.store);
    NetworkSecrets accepted{};
    TEST_ASSERT_TRUE(secrets.load_wifi_for_network(accepted));
    TEST_ASSERT_TRUE(std::string(accepted.ssid.data()) == new_ssid);
    TEST_ASSERT_TRUE(std::string(accepted.password.data()) == new_password);
    manager.poll();
    TEST_ASSERT_EQUAL_INT(1, backend.wifi_writes);
    TEST_ASSERT_TRUE(manager.request_candidate(bad_ssid.c_str(), bad_password.c_str()));
    backend.store.fail_next(MemorySecretBlobStore::Fault::WriteWifi);
    station.online = true;
    manager.poll();
    TEST_ASSERT_EQUAL_INT(WifiState::Backoff, manager.state());
    TEST_ASSERT_EQUAL_INT(2, backend.wifi_writes);
    const auto failed_blob = wifi_bytes(backend.store);
    TEST_ASSERT_EQUAL_UINT(accepted_blob.size, failed_blob.size);
    TEST_ASSERT_TRUE(accepted_blob.bytes == failed_blob.bytes);
    NetworkSecrets preserved{};
    TEST_ASSERT_TRUE(secrets.load_wifi_for_network(preserved));
    TEST_ASSERT_TRUE(preserved.ssid == accepted.ssid);
    TEST_ASSERT_TRUE(preserved.password == accepted.password);
    TEST_ASSERT_EQUAL_INT(0, backend.token_writes);
    TEST_ASSERT_EQUAL_INT(0, backend.token_reads);
    TEST_ASSERT_EQUAL_INT(3, station.begin_calls);
    TEST_ASSERT_EQUAL_INT(3, station.disconnect_calls);
    TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
        manager.request_scan(WifiScanSource::Operator));
    station.scan_result = WifiScanStatus::Pending;
    std::array<WifiNetwork, 2> rows{};
    std::size_t count{99U};
    TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending,
        manager.poll_scan(rows.data(), rows.size(), count));
    TEST_ASSERT_EQUAL_UINT(0U, count);
    TEST_ASSERT_EQUAL_INT(WifiScanStatus::Pending, manager.request_scan());
    station.scan_result = WifiScanStatus::Ready;
    ++clock.now;
    TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
        manager.poll_scan(rows.data(), rows.size(), count));
    TEST_ASSERT_EQUAL_UINT(1U, count);
    TEST_ASSERT_EQUAL_INT(WifiScanStatus::RateLimited, manager.request_scan());
    TEST_ASSERT_EQUAL_INT(1, station.scan_begin_calls);
    TEST_ASSERT_EQUAL_INT(2, station.scan_poll_calls);
    const auto attempts = mode < 3U ? sink.attempts : forwarding.attempts;
    TEST_ASSERT_EQUAL_INT(WifiScanStatus::Ready,
        manager.list_known(rows.data(), rows.size(), count));
    manager.poll();
    const auto cursor = sigap::debug::Cursor{boot, service.meta(clock.now).latest};
    (void)service.read(cursor);
    sigap::debug::BatchSnapshot snapshot{};
    TEST_ASSERT_TRUE(service.capture(cursor, 8U, clock.now, snapshot));
    TEST_ASSERT_EQUAL_UINT(attempts, mode < 3U ? sink.attempts : forwarding.attempts);
    TEST_ASSERT_EQUAL_INT(2, backend.wifi_writes);
    if (mode == 0U) TEST_ASSERT_EQUAL_UINT(0U, sink.attempts);
    if (mode == 1U) {
      TEST_ASSERT_TRUE(sink.attempts > 0U);
      TEST_ASSERT_EQUAL_UINT(sink.attempts, sink.size);
      for (std::size_t i = 0U; i < sink.size; ++i) {
        const auto& event = sink.events[i];
        assert_wifi_input(event, event.event_code, event.arg0, event.arg1, event.monotonic_us);
      }
    }
    if (mode == 2U) {
      TEST_ASSERT_TRUE(sink.attempts > 0U);
      TEST_ASSERT_EQUAL_UINT(0U, sink.size);
      assert_wifi_input(sink.last, 23U, 15, 0, clock.now);
    }
    if (mode == 3U || mode == 4U) {
      TEST_ASSERT_TRUE(forwarding.attempts > 0U);
      const auto meta = service.meta(clock.now);
      if (mode == 3U) {
        TEST_ASSERT_EQUAL_UINT(0U, forwarding.accepted);
        TEST_ASSERT_EQUAL_UINT64(meta_before.latest, meta.latest);
        TEST_ASSERT_EQUAL_UINT64(meta_before.overwritten, meta.overwritten);
      } else {
        TEST_ASSERT_EQUAL_UINT(forwarding.attempts, forwarding.accepted);
        TEST_ASSERT_EQUAL_UINT64(meta_before.latest + forwarding.accepted, meta.latest);
        TEST_ASSERT_EQUAL_UINT64(meta_before.overwritten + forwarding.accepted, meta.overwritten);
        const auto last = service.read({boot, meta.latest - 1U});
        TEST_ASSERT_TRUE(last.has_event);
        TEST_ASSERT_EQUAL_UINT(23U, last.event.event_code);
        TEST_ASSERT_EQUAL_INT(sigap::debug::Module::Wifi, last.event.module);
      }
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_saved_wifi_load_ignores_absent_or_corrupt_api_token);
  RUN_TEST(test_wifi_startup_uses_wifi_only_load_without_token_access);
  RUN_TEST(test_candidate_failure_preserves_saved_credentials);
  RUN_TEST(test_successful_candidate_persists_after_ip);
  RUN_TEST(test_scan_is_rate_limited);
  RUN_TEST(test_empty_known_network_list_is_ready_before_first_provisioning);
  RUN_TEST(test_wifi_scan_events_report_observed_lifecycle);
  RUN_TEST(test_wifi_scan_unknown_failure_does_not_invent_reason);
  RUN_TEST(test_wifi_link_retry_cycles_emit_only_real_state_changes);
  RUN_TEST(test_pending_scan_polling_is_quiet_and_zero_ready_is_reported);
  RUN_TEST(test_candidate_credentials_write_only_after_ip_and_only_once);
  RUN_TEST(test_operator_cancel_uses_normal_diagnostic_reason);
  RUN_TEST(test_wifi_write_fault_preserves_saved_credentials_and_never_reports_up);
  RUN_TEST(test_scan_origins_and_attempts_follow_real_admission);
  RUN_TEST(test_scan_clock_boundaries_and_saturated_observed_duration);
  RUN_TEST(test_large_scan_policy_uses_wide_retry_arithmetic);
  RUN_TEST(test_scan_failure_provenance_is_not_inferred_or_carried_into_other_statuses);
  RUN_TEST(test_two_association_failure_deadline_retry_up_cycles_preserve_saved_bytes);
  RUN_TEST(test_exact_down_states_and_association_timeout_are_observable_without_duplicate_polls);
  RUN_TEST(test_sink_backpressure_cannot_change_scan_link_or_persistence_business_effects);
  return UNITY_END();
}
