#include <cstring>
#include <cstdio>
#include <string>

#include "provisioning/ble_provisioning.h"
#include "provisioning/provisioning_secret_store.h"
#include "unity.h"

using namespace sigap::provisioning;

namespace {
std::string response_payload(const BleResponse& response) {
  const std::string frame(response.bytes.data(), response.size);
  const auto first = frame.find('|');
  const auto second = frame.find('|', first + 1U);
  const auto third = frame.find('|', second + 1U);
  const auto fourth = frame.find('|', third + 1U);
  return fourth == std::string::npos ? frame : frame.substr(fourth + 1U);
}

std::string gather_response(BleProvisioningSession& session,
                            const BleResponse& first) {
  std::string result = response_payload(first);
  for (std::size_t i = 0U; i < 256U; ++i) {
    BleResponse next{};
    session.poll(next);
    if (!next.notify) break;
    result += response_payload(next);
  }
  return result;
}

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
        count = std::min(scan_result_count, capacity);
        for (std::size_t i = 0U; i < count; ++i) {
          std::snprintf(output[i].ssid.data(), output[i].ssid.size(),
                        scan_result_count == 1U ? "SIGAP" : "AP%u",
                        static_cast<unsigned>(i));
          output[i].rssi = -41 - static_cast<int>(i);
          output[i].secure = true;
        }
      }
      return WifiScanStatus::Ready;
    }
    return scan_status;
  }
  WifiScanStatus list_known_wifi(WifiNetwork* output, std::size_t capacity,
                                 std::size_t& count) noexcept override {
    count = 0U;
    if (output == nullptr || capacity == 0U) return WifiScanStatus::Idle;
    std::strcpy(output[0].ssid.data(), "saved-network");
    output[0].secure = true;
    count = 1U;
    return WifiScanStatus::Ready;
  }
  bool request_connect(std::string_view ssid,
                       std::string_view password) noexcept override {
    connected_ssid.assign(ssid);
    connected_password.assign(password);
    connect_calls++;
    return true;
  }
  bool retry_wifi() noexcept override {
    retry_calls++;
    return true;
  }
  void cancel() noexcept override { cancel_calls++; }

  WifiState state{WifiState::SetupRequired};
  WifiScanStatus scan_status{WifiScanStatus::Pending};
  int scan_calls{0};
  std::size_t scan_result_count{1U};
  int connect_calls{0};
  int retry_calls{0};
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
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        BleCommandParser::parse("GET_STATUS", command));
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        BleCommandParser::parse("LIST_KNOWN_WIFI", command));
  TEST_ASSERT_EQUAL_INT(BleCommandKind::ListKnownWifi, command.kind);
}

void test_framed_commands_require_order_and_reassemble() {
  BleWireFrame frame{};
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        BleCommandParser::decode_frame("F1|42|0|2|GET_", frame));
  TEST_ASSERT_TRUE(frame.framed);
  TEST_ASSERT_EQUAL_UINT32(42U, frame.request_id);
  TEST_ASSERT_EQUAL_UINT16(2U, frame.chunk_count);

  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(0U);
  session.on_authenticated(true);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("F1|42|0|2|GET_", response));
  TEST_ASSERT_TRUE(response.size <= 20U);
  TEST_ASSERT_NOT_NULL(std::strstr(response_payload(response).c_str(), "CHUNK"));
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
  session.receive("F1|42|1|2|STATUS", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "STATUS"));
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,
                        session.receive("F1|43|1|2|STATUS", response));
}

void test_known_wifi_and_retry_commands_are_bounded() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(0U);
  session.on_authenticated(true);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("LIST_KNOWN_WIFI", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "saved-network"));
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("RETRY_WIFI", response));
  TEST_ASSERT_EQUAL_INT(1, backend.retry_calls);
}

void test_completed_request_id_cannot_be_replayed() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(0U);
  session.on_authenticated(true);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("F1|7|0|1|RETRY_WIFI", response));
  (void)gather_response(session, response);
  TEST_ASSERT_EQUAL_INT(1, backend.retry_calls);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,
                        session.receive("F1|7|0|1|RETRY_WIFI", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "REPLAY"));
  TEST_ASSERT_EQUAL_INT(1, backend.retry_calls);
}

void test_session_requires_encrypted_authenticated_link() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(clock.now);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,
                        session.receive("STATUS", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "AUTH_REQUIRED"));
  session.on_authenticated(true);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("STATUS", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "SETUP_REQUIRED"));
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
  const auto scan = gather_response(session, response);
  TEST_ASSERT_NOT_NULL(std::strstr(scan.c_str(), "PENDING"));
  TEST_ASSERT_NOT_NULL(std::strstr(scan.c_str(), "SIGAP"));
  TEST_ASSERT_EQUAL_INT(1, backend.scan_calls);
}

void test_scan_response_count_matches_serialized_entries() {
  Backend backend;
  backend.scan_result_count = 16U;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(0U);
  session.on_authenticated(true);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("SCAN_WIFI", response));
  const auto scan = gather_response(session, response);
  const auto ready_offset = scan.find("OK SCAN READY");
  TEST_ASSERT_NOT_EQUAL(std::string::npos, ready_offset);
  unsigned reported{0U};
  TEST_ASSERT_EQUAL_INT(1, std::sscanf(scan.c_str() + ready_offset,
                                       "OK SCAN READY count=%u", &reported));
  const auto first_line = scan.find('\n', ready_offset);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, first_line);
  unsigned entries{0U};
  for (std::size_t cursor = first_line; cursor != std::string::npos;
       cursor = scan.find('\n', cursor + 1U)) {
    ++entries;
  }
  TEST_ASSERT_EQUAL_UINT(entries, reported);
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
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "CONNECTING"));
  session.receive("STATUS", response);
  TEST_ASSERT_NULL(std::strstr(gather_response(session, response).c_str(), "secret-password"));
}

void test_provisioning_secret_is_separate_and_stable() {
  MemoryProvisioningSecretStore store;
  std::array<uint8_t, kProvisioningSecretBytes> first{};
  std::array<uint8_t, kProvisioningSecretBytes> second{};
  TEST_ASSERT_TRUE(store.load_or_create(first));
  TEST_ASSERT_TRUE(store.load_or_create(second));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.data(), second.data(), first.size());
  store.fail_next();
  TEST_ASSERT_FALSE(store.load_or_create(second));
}

void test_local_passkey_handoff_is_one_shot_and_bounded() {
  LocalPasskeyHandoff handoff;
  char output[7]{};
  TEST_ASSERT_EQUAL_INT(
      PasskeyDisclosureStatus::Unavailable,
      handoff.disclose_once(output, sizeof(output)));
  handoff.configure(42U);
  TEST_ASSERT_EQUAL_INT(PasskeyDisclosureStatus::Ready,
                        handoff.disclose_once(output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("000042", output);
  TEST_ASSERT_EQUAL_INT(
      PasskeyDisclosureStatus::AlreadyDisclosed,
      handoff.disclose_once(output, sizeof(output)));
  char short_output[6]{};
  handoff.configure(123456U);
  TEST_ASSERT_EQUAL_INT(
      PasskeyDisclosureStatus::Unavailable,
      handoff.disclose_once(short_output, sizeof(short_output)));
  TEST_ASSERT_EQUAL_INT(
      PasskeyDisclosureStatus::Ready,
      handoff.disclose_once(output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("123456", output);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parser_accepts_bounded_commands);
  RUN_TEST(test_framed_commands_require_order_and_reassemble);
  RUN_TEST(test_known_wifi_and_retry_commands_are_bounded);
  RUN_TEST(test_completed_request_id_cannot_be_replayed);
  RUN_TEST(test_session_requires_encrypted_authenticated_link);
  RUN_TEST(test_scan_is_bounded_and_returns_result);
  RUN_TEST(test_scan_response_count_matches_serialized_entries);
  RUN_TEST(test_disconnect_cancels_and_timeout_expires_session);
  RUN_TEST(test_connect_never_exposes_password_in_status);
  RUN_TEST(test_provisioning_secret_is_separate_and_stable);
  RUN_TEST(test_local_passkey_handoff_is_one_shot_and_bounded);
  return UNITY_END();
}
