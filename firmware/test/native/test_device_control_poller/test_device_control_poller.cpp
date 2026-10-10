#include <array>
#include <cstring>

#include <unity.h>

#include "network/device_control.h"
#include "network/device_control_codec.h"

namespace {

using namespace sigap::network;

struct Clock final : IMonotonicClock {
  uint64_t now{0U};
  uint64_t now_us() const noexcept override { return now; }
};

struct Transport final : IDeviceControlTransport {
  DeviceControlCommand command{};
  int command_status{200};
  int result_status{200};
  bool command_available{true};
  uint32_t pending_command_polls{0U};
  uint32_t pending_result_polls{0U};
  uint32_t command_starts{0U};
  uint32_t result_starts{0U};
  uint32_t result_polls{0U};

  TransportResult start_next_command(std::string_view device,
                                     std::string_view token, uint32_t,
                                     DeviceControlHttpResponse& response) noexcept override {
    ++command_starts;
    TEST_ASSERT_EQUAL_STRING("DEV-1", device.data());
    TEST_ASSERT_EQUAL_STRING("token", token.data());
    if (pending_command_polls != 0U) return TransportResult::Pending;
    response.status_code = command_status;
    response.command_present = command_available;
    response.command = command;
    return TransportResult::Response;
  }

  TransportResult poll_next_command(DeviceControlHttpResponse& response) noexcept override {
    ++result_polls;
    if (pending_command_polls != 0U) {
      --pending_command_polls;
      return TransportResult::Pending;
    }
    response.status_code = command_status;
    response.command_present = command_available;
    response.command = command;
    return TransportResult::Response;
  }

  TransportResult start_command_result(std::string_view device,
                                       std::string_view token,
                                       const DeviceControlCommand& value,
                                       const DeviceControlResult& result,
                                       uint32_t,
                                       DeviceControlHttpResponse& response) noexcept override {
    ++result_starts;
    TEST_ASSERT_EQUAL_STRING("DEV-1", device.data());
    TEST_ASSERT_EQUAL_STRING("token", token.data());
    TEST_ASSERT_EQUAL_STRING(value.command_id.bytes.data(), result.command_id.bytes.data());
    if (pending_result_polls != 0U) return TransportResult::Pending;
    response.status_code = result_status;
    return TransportResult::Response;
  }

  TransportResult poll_command_result(DeviceControlHttpResponse& response) noexcept override {
    ++result_polls;
    if (pending_result_polls != 0U) {
      --pending_result_polls;
      return TransportResult::Pending;
    }
    response.status_code = result_status;
    return TransportResult::Response;
  }

  void cancel_command_request() noexcept override {}
};

struct Executor final : IDeviceControlExecutor {
  uint32_t calls{0U};
  bool success{true};
  bool execute(const DeviceControlCommand& command, uint64_t,
               DeviceControlResult& result) noexcept override {
    ++calls;
    result.status = success ? DeviceControlStatus::Committed
                            : DeviceControlStatus::Failed;
    result.payload_size = 2U;
    result.payload[0] = '{';
    result.payload[1] = '}';
    return success;
  }
};

struct Journal final : IDeviceControlResultJournal {
  DeviceControlCommand command{};
  DeviceControlResult result{};
  bool present{false};
  bool fail_save{false};
  bool fail_clear{false};
  uint32_t save_calls{0U};
  uint32_t clear_calls{0U};
  bool save(const DeviceControlCommand& value,
            const DeviceControlResult& output) noexcept override {
    ++save_calls;
    if (fail_save) return false;
    command = value;
    result = output;
    present = true;
    return true;
  }
  bool load(DeviceControlCommand& value,
            DeviceControlResult& output) const noexcept override {
    if (!present) return false;
    value = command;
    output = result;
    return true;
  }
  bool clear() noexcept override {
    ++clear_calls;
    if (fail_clear) return false;
    present = false;
    return true;
  }
};

NetworkPolicy policy() {
  NetworkPolicy value{};
  value.retry_min_ms = 100U;
  value.retry_max_ms = 1000U;
  value.request_timeout_ms = 500U;
  value.retry_after_max_ms = 1000U;
  return value;
}

DeviceControlCommand make_command(uint64_t expiry = 10000U) {
  DeviceControlCommand value{};
  (void)value.command_id.assign("cmd-1");
  (void)value.device_id.assign("DEV-1");
  (void)value.operator_id.assign("op-1");
  (void)value.session_id.assign("session-1");
  (void)value.request_hash.assign("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  (void)value.sensor_id.assign("mq4");
  (void)value.operation.assign("commit");
  value.payload[0] = '{';
  value.payload[1] = '}';
  value.payload_size = 2U;
  value.expires_at_us = expiry;
  return value;
}

void test_stalled_get_and_result_do_not_block_or_repeat_execution() {
  Clock clock{};
  Transport transport{};
  transport.command = make_command();
  transport.pending_command_polls = 1U;
  transport.pending_result_polls = 1U;
  Executor executor{};
  DeviceControlPoller poller(policy(), clock, transport, executor);
  TEST_ASSERT_TRUE(poller.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Waiting, poller.poll());
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Waiting, poller.poll());
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Executed, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(1U, executor.calls);
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Waiting, poller.poll());
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Waiting, poller.poll());
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::ResultSent, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(1U, executor.calls);
}

void test_expired_or_wrong_device_command_is_never_executed() {
  Clock clock{};
  Transport transport{};
  transport.command = make_command(10U);
  Executor executor{};
  DeviceControlPoller poller(policy(), clock, transport, executor);
  TEST_ASSERT_TRUE(poller.begin("DEV-1", "token"));
  clock.now = 11U;
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Invalid, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(0U, executor.calls);
  transport.command = make_command();
  TEST_ASSERT_TRUE(transport.command.device_id.assign("DEV-2"));
  clock.now = poller.next_attempt_us();
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Invalid, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(0U, executor.calls);
}

void test_result_journal_recovers_lost_post_without_reexecution() {
  Clock clock{};
  Transport transport{};
  transport.command = make_command();
  Executor executor{};
  Journal journal{};
  DeviceControlPoller first(policy(), clock, transport, executor, &journal);
  TEST_ASSERT_TRUE(first.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Executed, first.poll());
  TEST_ASSERT_TRUE(journal.present);
  transport.result_status = 503;
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Failed, first.poll());

  Transport restarted_transport{};
  restarted_transport.result_status = 200;
  Executor restarted_executor{};
  DeviceControlPoller restarted(policy(), clock, restarted_transport,
                                restarted_executor, &journal);
  TEST_ASSERT_TRUE(restarted.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::ResultSent,
                        restarted.poll());
  TEST_ASSERT_EQUAL_UINT32(0U, restarted_executor.calls);
  TEST_ASSERT_FALSE(journal.present);
}

void test_recovered_result_ignores_prior_boot_monotonic_deadline() {
  Clock clock{};
  Journal journal{};
  journal.command = make_command(10U);
  journal.result.status = DeviceControlStatus::Committed;
  TEST_ASSERT_TRUE(journal.result.command_id.assign("cmd-1"));
  TEST_ASSERT_TRUE(journal.result.session_id.assign("session-1"));
  journal.result.payload[0] = '{';
  journal.result.payload[1] = '}';
  journal.result.payload_size = 2U;
  journal.present = true;
  // A reboot-relative clock cannot be compared with a deadline from the prior
  // boot. The already executed result must be reconciled, not discarded.
  clock.now = 1000U;
  Transport transport{};
  transport.result_status = 200;
  Executor executor{};
  DeviceControlPoller poller(policy(), clock, transport, executor, &journal);
  TEST_ASSERT_TRUE(poller.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::ResultSent, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(0U, executor.calls);
  TEST_ASSERT_FALSE(journal.present);
}

void test_journal_save_failure_blocks_new_commands_until_durable() {
  Clock clock{};
  Transport transport{};
  transport.command = make_command();
  Executor executor{};
  Journal journal{};
  journal.fail_save = true;
  DeviceControlPoller poller(policy(), clock, transport, executor, &journal);
  TEST_ASSERT_TRUE(poller.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Failed, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(1U, executor.calls);
  TEST_ASSERT_EQUAL_UINT32(1U, transport.command_starts);

  clock.now = poller.next_attempt_us();
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Failed, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(1U, executor.calls);
  TEST_ASSERT_EQUAL_UINT32(1U, transport.command_starts);

  journal.fail_save = false;
  clock.now = poller.next_attempt_us();
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Executed, poller.poll());
  TEST_ASSERT_TRUE(journal.present);
  TEST_ASSERT_EQUAL_UINT32(1U, executor.calls);
  TEST_ASSERT_EQUAL_UINT32(1U, transport.command_starts);
}

void test_journal_clear_failure_blocks_new_commands() {
  Clock clock{};
  Transport transport{};
  transport.command = make_command();
  Executor executor{};
  Journal journal{};
  journal.fail_clear = true;
  DeviceControlPoller poller(policy(), clock, transport, executor, &journal);
  TEST_ASSERT_TRUE(poller.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Executed, poller.poll());
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Failed, poller.poll());
  TEST_ASSERT_TRUE(journal.present);
  TEST_ASSERT_EQUAL_UINT32(1U, transport.command_starts);

  clock.now = poller.next_attempt_us();
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Failed, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(1U, transport.command_starts);

  journal.fail_clear = false;
  clock.now = poller.next_attempt_us();
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::ResultSent, poller.poll());
  TEST_ASSERT_FALSE(journal.present);
  TEST_ASSERT_EQUAL_UINT32(1U, transport.command_starts);
}

void test_unauthorized_state_requires_explicit_reauthorization() {
  Clock clock{};
  Transport transport{};
  transport.command_status = 401;
  Executor executor{};
  DeviceControlPoller poller(policy(), clock, transport, executor);
  TEST_ASSERT_TRUE(poller.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Unauthorized, poller.poll());
  transport.command_status = 200;
  transport.command_available = false;
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Unauthorized, poller.poll());
  poller.authorize_again();
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Idle, poller.poll());
}

void test_relative_server_expiry_is_bounded_before_execution() {
  uint64_t deadline{0U};
  TEST_ASSERT_TRUE(device_control_deadline_from_relative_ms(
      500U, 1000U, deadline));
  TEST_ASSERT_EQUAL_UINT64(1000500U, deadline);
  TEST_ASSERT_FALSE(device_control_deadline_from_relative_ms(
      500U, 0U, deadline));
  TEST_ASSERT_FALSE(device_control_deadline_from_relative_ms(
      500U, kMaxDeviceControlExpiryMs + 1U, deadline));
  TEST_ASSERT_FALSE(device_control_deadline_from_relative_ms(
      UINT64_MAX - 10U, 1U, deadline));
}

void test_oversized_command_never_reaches_executor() {
  Clock clock{};
  Transport transport{};
  transport.command = make_command();
  transport.command.payload_size = kMaxDeviceControlPayloadBytes + 1U;
  Executor executor{};
  DeviceControlPoller poller(policy(), clock, transport, executor);
  TEST_ASSERT_TRUE(poller.begin("DEV-1", "token"));
  TEST_ASSERT_EQUAL_INT(DeviceControlPollOutcome::Invalid, poller.poll());
  TEST_ASSERT_EQUAL_UINT32(0U, executor.calls);
}

void test_wire_command_requires_bounded_relative_expiry() {
  constexpr std::string_view valid =
      R"({"command_id":"cmd-1","device_id":"DEV-1","operator_id":"op-1","session_id":"s-1","sensor_id":"mq4","operation":"start","payload":{"samples":3},"expires_in_ms":1000,"attempt":2,"request_hash":"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})";
  DeviceControlCommand command{};
  TEST_ASSERT_TRUE(decode_device_control_command(valid, 500U, command));
  TEST_ASSERT_EQUAL_STRING("cmd-1", command.command_id.bytes.data());
  TEST_ASSERT_EQUAL_UINT64(1000500U, command.expires_at_us);
  TEST_ASSERT_EQUAL_UINT32(2U, command.attempt);

  constexpr std::string_view no_deadline =
      R"({"command_id":"cmd-1","device_id":"DEV-1","operator_id":"op-1","session_id":"s-1","sensor_id":"mq4","operation":"start","payload":{},"request_hash":"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})";
  TEST_ASSERT_FALSE(decode_device_control_command(no_deadline, 500U, command));
}

void test_wire_result_rejects_oversized_or_invalid_payload() {
  DeviceControlResult result{};
  TEST_ASSERT_TRUE(result.command_id.assign("cmd-1"));
  TEST_ASSERT_TRUE(result.session_id.assign("s-1"));
  result.status = DeviceControlStatus::Committed;
  constexpr std::string_view committed =
      R"({"profile_key":"mq4","profile_hash":"sha256:abc","version":1})";
  result.payload_size = committed.size();
  std::copy(committed.begin(), committed.end(), result.payload.begin());
  DeviceControlWireBody body{};
  TEST_ASSERT_TRUE(encode_device_control_result(result, body));
  TEST_ASSERT_TRUE(body.view().find("COMMITTED") != std::string_view::npos);
  result.payload_size = result.payload.size() + 1U;
  TEST_ASSERT_FALSE(encode_device_control_result(result, body));
}

void test_canonical_uint64_profile_versions_are_exact() {
  uint64_t value{0U};
  TEST_ASSERT_TRUE(parse_canonical_uint64("18446744073709551615", value));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, value);
  TEST_ASSERT_TRUE(parse_canonical_uint64("1", value));
  TEST_ASSERT_EQUAL_UINT64(1U, value);
  TEST_ASSERT_FALSE(parse_canonical_uint64("0", value));
  TEST_ASSERT_FALSE(parse_canonical_uint64("01", value));
  TEST_ASSERT_FALSE(parse_canonical_uint64("-1", value));
  TEST_ASSERT_FALSE(parse_canonical_uint64("18446744073709551616", value));
  TEST_ASSERT_FALSE(parse_canonical_uint64("1.0", value));
}

void test_executor_profile_version_field_requires_one_json_string() {
  uint64_t value{0U};
  TEST_ASSERT_TRUE(parse_json_canonical_uint64_field(
      R"({"expected_profile_version":"18446744073709551615"})",
      "expected_profile_version", value));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, value);
  TEST_ASSERT_FALSE(parse_json_canonical_uint64_field(
      R"({"expected_profile_version":18446744073709551615})",
      "expected_profile_version", value));
  TEST_ASSERT_FALSE(parse_json_canonical_uint64_field(
      R"({"expected_profile_version":"1","expected_profile_version":"2"})",
      "expected_profile_version", value));
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_stalled_get_and_result_do_not_block_or_repeat_execution);
  RUN_TEST(test_expired_or_wrong_device_command_is_never_executed);
  RUN_TEST(test_result_journal_recovers_lost_post_without_reexecution);
  RUN_TEST(test_recovered_result_ignores_prior_boot_monotonic_deadline);
  RUN_TEST(test_journal_save_failure_blocks_new_commands_until_durable);
  RUN_TEST(test_journal_clear_failure_blocks_new_commands);
  RUN_TEST(test_unauthorized_state_requires_explicit_reauthorization);
  RUN_TEST(test_relative_server_expiry_is_bounded_before_execution);
  RUN_TEST(test_oversized_command_never_reaches_executor);
  RUN_TEST(test_wire_command_requires_bounded_relative_expiry);
  RUN_TEST(test_wire_result_rejects_oversized_or_invalid_payload);
  RUN_TEST(test_canonical_uint64_profile_versions_are_exact);
  RUN_TEST(test_executor_profile_version_field_requires_one_json_string);
  UNITY_END();
}

void loop() {}

int main() {
  setup();
  return 0;
}
