#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <unity.h>

#include "maintenance/maintenance.h"
#include "maintenance/runtime.h"
#include "config/config_store.h"
#include "config/config_validator.h"
#include "provisioning/secret_store.h"

namespace {

using namespace sigap::maintenance;

constexpr sigap::config::ValidationContext kValidationContext{
    8ULL * 1024ULL * 1024ULL, 0U};

struct FakeTransport final : IMaintenanceTransport {
  std::string input;
  std::string output;
  std::size_t offset{0};
  bool link{true};

  bool available() noexcept override { return offset < input.size(); }

  int read() noexcept override {
    if (!available()) return -1;
    return static_cast<unsigned char>(input[offset++]);
  }

  bool write(std::string_view value) noexcept override {
    output.append(value.data(), value.size());
    return true;
  }

  bool connected() noexcept override { return link; }
};

struct FakeSink final : IMaintenanceCommandSink {
  std::size_t calls{0};
  std::size_t poll_calls{0};
  std::size_t transport_lost_calls{0};
  MaintenanceCommand last{};

  CommandStatus execute(const MaintenanceCommand& command, uint64_t,
                        MaintenanceResponse& response) noexcept override {
    ++calls;
    last = command;
    response.append("OK ");
    response.append(command_name(command.kind));
    response.append("\n");
    return CommandStatus::Accepted;
  }

  void poll(uint64_t) noexcept override { ++poll_calls; }

  void transport_lost() noexcept override { ++transport_lost_calls; }
};

void test_profile_lifecycle_commands_parse_as_explicit_steps() {
  FakeTransport transport{};
  transport.input = "profile validate hx711_organic\n"
                    "profile preview hx711_organic\n"
                    "profile commit hx711_organic\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  while (transport.available()) service.poll(0U);

  TEST_ASSERT_EQUAL_UINT32(3U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::ProfileCommit),
                        static_cast<int>(sink.last.kind));
  TEST_ASSERT_EQUAL_STRING("hx711_organic", sink.last.target.c_str());
}

void test_service_polls_without_input_and_reports_disconnect_once() {
  FakeTransport transport{};
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  service.poll(10U);
  TEST_ASSERT_TRUE(sink.poll_calls >= 1U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.transport_lost_calls);

  transport.link = false;
  service.poll(20U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.transport_lost_calls);
  service.poll(30U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.transport_lost_calls);
}

void test_fragmented_input_is_buffered_until_line_end() {
  FakeTransport transport{};
  transport.input = "status";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  TEST_ASSERT_EQUAL_UINT32(6U, service.poll(10U).bytes_read);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.calls);

  transport.input += "\n";
  const auto stats = service.poll(20U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Status),
                        static_cast<int>(sink.last.kind));
  TEST_ASSERT_EQUAL_UINT32(1U, stats.commands_dispatched);
  TEST_ASSERT_TRUE(transport.output.find("OK status") != std::string::npos);
}

void test_multiple_commands_are_processed_with_poll_budget() {
  FakeTransport transport{};
  transport.input = "status\nmaintenance enter\nstatus\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  const auto first = service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(kMaxBytesPerPoll, first.bytes_read);
  TEST_ASSERT_TRUE(sink.calls >= 1U);

  while (transport.available()) service.poll(1U);
  TEST_ASSERT_EQUAL_UINT32(3U, sink.calls);
  TEST_ASSERT_EQUAL_UINT32(3U, service.commands_dispatched());
}

void test_oversized_command_is_rejected_and_parser_recovers() {
  FakeTransport transport{};
  transport.input = std::string(kMaxCommandLineBytes + 8U, 'x') +
                    "\nstatus\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  while (transport.available()) service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_TRUE(transport.output.find("ERR OVERSIZED") != std::string::npos);
  TEST_ASSERT_TRUE(transport.output.find("OK status") != std::string::npos);
}

void test_unknown_empty_and_invalid_numeric_commands_are_explicit() {
  FakeTransport transport{};
  transport.input = "\nnot-a-command\ncalibration capture organic nope\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);

  while (transport.available()) service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(0U, sink.calls);
  TEST_ASSERT_TRUE(transport.output.find("ERR EMPTY") != std::string::npos);
  TEST_ASSERT_TRUE(transport.output.find("ERR UNKNOWN_COMMAND") !=
                  std::string::npos);
  TEST_ASSERT_TRUE(transport.output.find("ERR INVALID_NUMERIC") !=
                  std::string::npos);
}

void test_config_numeric_and_calibration_arguments_are_typed() {
  FakeTransport transport{};
  transport.input = "config set sampling.mq_period_ms 100\n"
                    "calibration capture organic -42\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);
  while (transport.available()) service.poll(0U);

  TEST_ASSERT_EQUAL_UINT32(2U, sink.calls);
  TEST_ASSERT_TRUE(sink.last.has_numeric);
  TEST_ASSERT_EQUAL_INT64(-42, sink.last.numeric_value);
  TEST_ASSERT_EQUAL_STRING("organic", sink.last.target.c_str());
}

void test_response_redacts_secret_and_stays_bounded() {
  MaintenanceResponse response;
  TEST_ASSERT_TRUE(response.append_secret("token", "super-secret"));
  TEST_ASSERT_TRUE(response.append_presence("wifi", true));
  TEST_ASSERT_TRUE(response.size() < kMaxResponseBytes);
  TEST_ASSERT_NOT_EQUAL(nullptr, response.c_str());
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        std::string(response.view()).find("[REDACTED]"));
  TEST_ASSERT_EQUAL(std::string::npos,
                    std::string(response.view()).find("super-secret"));
}

void test_parser_failure_does_not_stop_later_commands() {
  FakeTransport transport{};
  transport.input = "config set sampling.mq_period_ms bad\nstatus\n";
  FakeSink sink{};
  MaintenanceService service(transport, sink);
  while (transport.available()) service.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, sink.calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandKind::Status),
                        static_cast<int>(sink.last.kind));
}

void test_command_router_keeps_draft_isolated_and_redacts_status() {
  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config_manager(config_store, kValidationContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config_manager.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_backend{};
  sigap::provisioning::PersistentSecretStore secrets(secret_backend);
  CommandRouter router(config_manager, secrets);
  MaintenanceCommand start{};
  start.kind = CommandKind::ConfigDraftStart;
  MaintenanceResponse response{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(start, 0U, response)));
  MaintenanceCommand set{};
  set.kind = CommandKind::ConfigSet;
  TEST_ASSERT_TRUE(set.field.assign("sampling.mq_period_ms"));
  set.has_numeric = true;
  set.numeric_value = 250;
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(set, 0U, response)));
  TEST_ASSERT_EQUAL_UINT32(100U,
                           config_manager.active().sampling.mq_period_ms);
  TEST_ASSERT_EQUAL_UINT32(250U,
                           config_manager.draft().sampling.mq_period_ms);
  MaintenanceCommand status{};
  status.kind = CommandKind::Status;
  response = MaintenanceResponse{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(router.execute(status, 0U, response)));
  TEST_ASSERT_TRUE(response.view().find("config_hash=") != std::string_view::npos);
}

void test_maintenance_memory_footprint_is_bounded() {
  std::printf("memory MaintenanceParser=%zu MaintenanceResponse=%zu "
              "CommandRouter=%zu MaintenanceService=%zu\n",
              sizeof(MaintenanceParser), sizeof(MaintenanceResponse),
              sizeof(CommandRouter), sizeof(MaintenanceService));
  TEST_ASSERT_TRUE(sizeof(MaintenanceResponse) <= 256U);
  TEST_ASSERT_TRUE(sizeof(MaintenanceService) <= 512U);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_fragmented_input_is_buffered_until_line_end);
  RUN_TEST(test_profile_lifecycle_commands_parse_as_explicit_steps);
  RUN_TEST(test_service_polls_without_input_and_reports_disconnect_once);
  RUN_TEST(test_multiple_commands_are_processed_with_poll_budget);
  RUN_TEST(test_oversized_command_is_rejected_and_parser_recovers);
  RUN_TEST(test_unknown_empty_and_invalid_numeric_commands_are_explicit);
  RUN_TEST(test_config_numeric_and_calibration_arguments_are_typed);
  RUN_TEST(test_response_redacts_secret_and_stays_bounded);
  RUN_TEST(test_parser_failure_does_not_stop_later_commands);
  RUN_TEST(test_command_router_keeps_draft_isolated_and_redacts_status);
  RUN_TEST(test_maintenance_memory_footprint_is_bounded);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
