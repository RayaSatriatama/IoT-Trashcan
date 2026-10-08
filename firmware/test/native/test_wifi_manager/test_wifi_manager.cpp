#include <string>
#include <cstring>

#include "provisioning/secret_store.h"
#include "provisioning/wifi_manager.h"
#include "unity.h"

using namespace sigap::provisioning;

namespace {
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
  bool begin_scan() noexcept override { scanning = true; return true; }
  WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                           std::size_t& count) noexcept override {
    if (!scanning) return WifiScanStatus::Idle;
    scanning = false;
    if (output != nullptr && capacity != 0U) {
      std::strcpy(output[0].ssid.data(), "AP");
      count = 1U;
    }
    return WifiScanStatus::Ready;
  }
  bool begin_ok{true};
  bool online{false};
  bool ip{true};
  bool failure{false};
  bool scanning{false};
  int begin_calls{0};
  int disconnect_calls{0};
  std::string requested_ssid;
  std::string requested_password;
};
}  // namespace

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

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_candidate_failure_preserves_saved_credentials);
  RUN_TEST(test_successful_candidate_persists_after_ip);
  RUN_TEST(test_scan_is_rate_limited);
  return UNITY_END();
}
