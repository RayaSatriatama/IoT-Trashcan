#include <cstring>
#include <cstdio>
#include <string>

#include "provisioning/ble_provisioning.h"

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

class CalibrationBackend final : public IBleCalibrationBackend {
 public:
  bool execute(std::string_view command, char* response, std::size_t capacity,
               std::size_t& response_size) noexcept override {
    last_command.assign(command);
    if (response == nullptr || capacity < 4U) return false;
    const char* value = command == "CAL STATUS ds18b20_b3"
                            ? "OK CAL STATUS sensor=ds18b20_b3 state=IDLE"
                            : "ERR CAL_REJECTED";
    response_size = std::min<std::size_t>(std::strlen(value), capacity);
    std::memcpy(response, value, response_size);
    return value[0] == 'O';
  }
  void disconnect() noexcept override { ++disconnects; }

  std::string last_command;
  int disconnects{0};
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
                        BleCommandParser::parse("GET_DEVICE_ID", command));
  TEST_ASSERT_EQUAL_INT(BleCommandKind::DeviceIdentity, command.kind);
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
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("F1|7|0|1|RETRY_WIFI", response));
  (void)gather_response(session, response);
  TEST_ASSERT_EQUAL_INT(1, backend.retry_calls);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,
                        session.receive("F1|7|0|1|RETRY_WIFI", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "REPLAY"));
  TEST_ASSERT_EQUAL_INT(1, backend.retry_calls);
}

void test_connected_session_accepts_commands_without_authentication() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(clock.now);

  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_STATUS", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(),
                                   "SETUP_REQUIRED"));
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("CONNECT lab password", response));
  TEST_ASSERT_EQUAL_INT(1, backend.connect_calls);
}

void test_reconnect_starts_a_fresh_unpaired_session() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  session.set_device_id(0x1234ULL);
  BleResponse response{};

  session.on_connect(clock.now);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_DEVICE_ID", response));
  TEST_ASSERT_EQUAL_STRING("OK DEVICE_ID id=0000000000001234",
                           gather_response(session, response).c_str());

  session.on_disconnect();
  session.on_connect(clock.now);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_DEVICE_ID", response));
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_DEVICE_ID", response));
  TEST_ASSERT_EQUAL_STRING("OK DEVICE_ID id=0000000000001234",
                           gather_response(session, response).c_str());
}

void test_device_identity_is_available_without_ble_authentication() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  session.set_device_id(0x1234ULL);
  BleResponse response{};
  session.on_connect(clock.now);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_DEVICE_ID", response));
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_DEVICE_ID", response));
  TEST_ASSERT_EQUAL_STRING("OK DEVICE_ID id=0000000000001234",
                           gather_response(session, response).c_str());
}

void test_identity_is_unavailable_when_application_has_not_bound_it() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(clock.now);
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_DEVICE_ID", response));
  TEST_ASSERT_EQUAL_STRING("ERR DEVICE_ID_UNAVAILABLE",
                           gather_response(session, response).c_str());
}

void test_scan_is_bounded_and_returns_result() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  BleResponse response{};
  session.on_connect(0U);
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
  clock.now = 11000U;
  session.poll(response);
  TEST_ASSERT_FALSE(session.connected());
  TEST_ASSERT_EQUAL_INT(1, backend.cancel_calls);
  session.on_connect(clock.now);
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
  session.receive("CONNECT private secret-password", response);
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(), "CONNECTING"));
  session.receive("STATUS", response);
  TEST_ASSERT_NULL(std::strstr(gather_response(session, response).c_str(), "secret-password"));
}


void test_calibration_commands_use_shared_bounded_backend() {
  BleCommand command{};
  TEST_ASSERT_EQUAL_INT(
      BleParseStatus::Accepted,
      BleCommandParser::parse("CAL STATUS ds18b20_b3", command));
  TEST_ASSERT_EQUAL_INT(BleCommandKind::CalibrationStatus, command.kind);
  TEST_ASSERT_EQUAL_INT(
      BleParseStatus::Accepted,
      BleCommandParser::parse("CAL STATUS", command));
  TEST_ASSERT_EQUAL_INT(
      BleParseStatus::Accepted,
      BleCommandParser::parse("CAL CAPABILITIES", command));

  Backend wifi;
  CalibrationBackend calibration;
  Clock clock;
  BleProvisioningSession session(wifi, clock);
  session.set_calibration_backend(&calibration);
  BleResponse response{};
  session.on_connect(0U);
  TEST_ASSERT_EQUAL_INT(
      BleParseStatus::Accepted,
      session.receive("CAL STATUS ds18b20_b3", response));
  TEST_ASSERT_NOT_NULL(std::strstr(gather_response(session, response).c_str(),
                                   "sensor=ds18b20_b3"));
  TEST_ASSERT_EQUAL_STRING("CAL STATUS ds18b20_b3", calibration.last_command.c_str());

  session.on_disconnect();
  TEST_ASSERT_EQUAL_INT(1, calibration.disconnects);
  TEST_ASSERT_EQUAL_INT(
      BleParseStatus::Invalid,
      session.receive("CAL STATUS ds18b20_b3", response));
}

void test_diagnostic_session_reports_wifi_disabled_and_allows_read_only_status() {
  Backend backend;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  session.set_read_only_diagnostic(true);
  session.set_device_id(0x98A316F5C150ULL);
  BleResponse response{};
  session.on_connect(0U);

  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_STATUS", response));
  TEST_ASSERT_EQUAL_STRING(
      "OK STATUS mode=BLE_DIAGNOSTIC wifi=DISABLED",
      gather_response(session, response).c_str());
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("GET_DEVICE_ID", response));
  TEST_ASSERT_EQUAL_STRING("OK DEVICE_ID id=000098a316f5c150",
                           gather_response(session, response).c_str());
  TEST_ASSERT_EQUAL_INT(BleParseStatus::Accepted,
                        session.receive("PING", response));
  TEST_ASSERT_EQUAL_STRING("OK PONG", gather_response(session, response).c_str());
}

void test_diagnostic_session_rejects_every_wifi_and_calibration_command() {
  Backend backend;
  CalibrationBackend calibration;
  Clock clock;
  BleProvisioningSession session(backend, clock);
  session.set_read_only_diagnostic(true);
  session.set_calibration_backend(&calibration);
  BleResponse response{};
  session.on_connect(0U);

  for (const char* command : {"SCAN", "CONNECT lab password", "RETRY_WIFI",
                              "LIST_KNOWN_WIFI", "CAL STATUS", "STATUS",
                              " GET_STATUS"}) {
    TEST_ASSERT_EQUAL_INT(BleParseStatus::Invalid,
                          session.receive(command, response));
    TEST_ASSERT_EQUAL_STRING("ERR DIAGNOSTIC_READ_ONLY",
                             gather_response(session, response).c_str());
  }
  TEST_ASSERT_EQUAL_INT(0, backend.scan_calls);
  TEST_ASSERT_EQUAL_INT(0, backend.connect_calls);
  TEST_ASSERT_EQUAL_INT(0, backend.retry_calls);
  TEST_ASSERT_EQUAL_INT(0, calibration.disconnects);
  TEST_ASSERT_TRUE(calibration.last_command.empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parser_accepts_bounded_commands);
  RUN_TEST(test_framed_commands_require_order_and_reassemble);
  RUN_TEST(test_known_wifi_and_retry_commands_are_bounded);
  RUN_TEST(test_completed_request_id_cannot_be_replayed);
  RUN_TEST(test_connected_session_accepts_commands_without_authentication);
  RUN_TEST(test_reconnect_starts_a_fresh_unpaired_session);
  RUN_TEST(test_device_identity_is_available_without_ble_authentication);
  RUN_TEST(test_identity_is_unavailable_when_application_has_not_bound_it);
  RUN_TEST(test_scan_is_bounded_and_returns_result);
  RUN_TEST(test_scan_response_count_matches_serialized_entries);
  RUN_TEST(test_disconnect_cancels_and_timeout_expires_session);
  RUN_TEST(test_connect_never_exposes_password_in_status);

  RUN_TEST(test_calibration_commands_use_shared_bounded_backend);
  RUN_TEST(test_diagnostic_session_reports_wifi_disabled_and_allows_read_only_status);
  RUN_TEST(test_diagnostic_session_rejects_every_wifi_and_calibration_command);
  return UNITY_END();
}
