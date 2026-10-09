#include <array>
#include <cstring>

#include <unity.h>

#include "network/device_control.h"

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
  bool save(const DeviceControlCommand& value,
            const DeviceControlResult& output) noexcept override {
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
  (void)value.session_id.assign("session-1");
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

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_stalled_get_and_result_do_not_block_or_repeat_execution);
  RUN_TEST(test_expired_or_wrong_device_command_is_never_executed);
  RUN_TEST(test_result_journal_recovers_lost_post_without_reexecution);
  RUN_TEST(test_unauthorized_state_requires_explicit_reauthorization);
  UNITY_END();
}

void loop() {}

int main() {
  setup();
  return 0;
}
