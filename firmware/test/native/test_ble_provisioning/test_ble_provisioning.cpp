#include <cstring>
#include <string>

#include "provisioning/ble_provisioning.h"
#include "unity.h"

using namespace sigap::provisioning;

namespace {
class Clock final : public BleProvisioningClock {
 public:
  uint64_t now_us() const noexcept override { return now; }
  uint64_t now{0U};
};

class Backend final : public IBleProvisioningBackend {
 public:
  WifiState wifi_state() const noexcept override { return state; }
  std::string_view current_ssid() const noexcept override { return "home"; }
  WifiScanStatus request_scan() noexcept override {
    scan_calls++;
    return scan_status;
  }
  WifiScanStatus poll_scan(WifiNetwork* output, std::size_t capacity,
                           std::size_t& count) noexcept override {
    if (scan_status == WifiScanStatus::Pending) {
      scan_status = WifiScanStatus::Ready;
      if (output != nullptr && capacity > 0U) {
        std::strcpy(output[0].ssid.data(), "SIGAP");
        output[0].rssi = -41;
        output[0].secure = true;
        count = 1U;
      }
      return WifiScanStatus::Ready;
    }
    return scan_status;
  }
  bool request_connect(std::string_view ssid,
                       std::string_view password) noexcept override {
    connected_ssid.assign(ssid);
    connected_password.assign(password);
    connect_calls++;
    return true;
  }
  void cancel() noexcept override { cancel_calls++; }

  WifiState state{WifiState::SetupRequired};
  WifiScanStatus scan_status{WifiScanStatus::Pending};
  int scan_calls{0};
  int connect_calls{0};
  int cancel_calls{0};
  std::string connected_ssid;
  std::string connected_password;
};
}  // namespace

void test_parser_accepts_bounded_commands() {
  BleCommand command{};
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        BleCommandParser::parse("CONNECT MyWifi pass word\n", command));
  TEST_ASSERT_EQUAL_UINT(6U, command.ssid_size);
  TEST_ASSERT_EQUAL_UINT(9U, command.password_size);
  TEST_ASSERT_EQUAL_INT(BleCommandKind::Connect, command.kind);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Oversized,
                        BleCommandParser::parse(std::string(193U, 'x'), command));
}

void test_session_requires_encrypted_authenticated_link() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(clock.now);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,
                        session.receive("STATUS", response));
  TEST_ASSERT_NOT_NULL(std::strstr(response.bytes.data(), "AUTH_REQUIRED"));
  session.on_authenticated(true);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("STATUS", response));
  TEST_ASSERT_NOT_NULL(std::strstr(response.bytes.data(), "SETUP_REQUIRED"));
}

void test_scan_is_bounded_and_returns_result() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(0U);
  session.on_authenticated(true);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("SCAN", response));
  TEST_ASSERT_NOT_NULL(std::strstr(response.bytes.data(), "PENDING"));
  session.poll(response);
  TEST_ASSERT_NOT_NULL(std::strstr(response.bytes.data(), "SIGAP"));
  TEST_ASSERT_EQUAL_INT(1, backend.scan_calls);
}

void test_disconnect_cancels_and_timeout_expires_session() {
  Backend backend;
  Clock clock;
  ProvisioningPolicy policy{};
  policy.session_timeout_ms = 10U;
  BleProvisioningSession session(backend, clock, policy);
  BleResponse response{};
  session.on_connect(0U);
  session.on_authenticated(true);
  clock.now = 11000U;
  session.poll(response);
  TEST_ASSERT_FALSE(session.connected());
  TEST_ASSERT_EQUAL_INT(1, backend.cancel_calls);
  session.on_connect(clock.now);
  session.on_authenticated(true);
  session.receive("CONNECT ssid password", response);
  session.on_disconnect();
  TEST_ASSERT_FALSE(session.connected());
  TEST_ASSERT_EQUAL_INT(2, backend.cancel_calls);
}

void test_connect_never_exposes_password_in_status() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(0U);
  session.on_authenticated(true);
  session.receive("CONNECT private secret-password", response);
  TEST_ASSERT_NOT_NULL(std::strstr(response.bytes.data(), "CONNECTING"));
  session.receive("STATUS", response);
  TEST_ASSERT_NULL(std::strstr(response.bytes.data(), "secret-password"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parser_accepts_bounded_commands);
  RUN_TEST(test_session_requires_encrypted_authenticated_link);
  RUN_TEST(test_scan_is_bounded_and_returns_result);
  RUN_TEST(test_disconnect_cancels_and_timeout_expires_session);
  RUN_TEST(test_connect_never_exposes_password_in_status);
  return UNITY_END();
}
