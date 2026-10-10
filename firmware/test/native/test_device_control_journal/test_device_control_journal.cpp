#include <cstdio>
#include <cstring>

#include <unity.h>

#include "network/device_control_journal.h"

namespace {

using namespace sigap::network;
constexpr const char* kPath = "/tmp/sigap-device-control-journal-test";

void remove_files() {
  (void)std::remove("/tmp/sigap-device-control-journal-test.a");
  (void)std::remove("/tmp/sigap-device-control-journal-test.b");
}

DeviceControlCommand command(const char* id = "cmd-1") {
  DeviceControlCommand value{};
  TEST_ASSERT_TRUE(value.command_id.assign(id));
  TEST_ASSERT_TRUE(value.device_id.assign("device-1"));
  TEST_ASSERT_TRUE(value.operator_id.assign("operator-1"));
  TEST_ASSERT_TRUE(value.session_id.assign("session-1"));
  TEST_ASSERT_TRUE(value.sensor_id.assign("mq4"));
  TEST_ASSERT_TRUE(value.operation.assign("commit"));
  TEST_ASSERT_TRUE(value.request_hash.assign(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  value.payload[0] = '{';
  value.payload[1] = '}';
  value.payload_size = 2U;
  value.expires_at_us = 4U;
  value.attempt = 2U;
  return value;
}

DeviceControlResult result(const char* id = "cmd-1") {
  DeviceControlResult value{};
  value.status = DeviceControlStatus::Committed;
  TEST_ASSERT_TRUE(value.command_id.assign(id));
  TEST_ASSERT_TRUE(value.session_id.assign("session-1"));
  value.payload[0] = '{';
  value.payload[1] = '1';
  value.payload[2] = '}';
  value.payload_size = 3U;
  return value;
}

void test_save_restart_loads_exact_result() {
  remove_files();
  const DeviceControlCommand expected_command = command();
  const DeviceControlResult expected_result = result();
  {
    NativeDeviceControlResultJournal journal(kPath);
    TEST_ASSERT_TRUE(journal.recover());
    TEST_ASSERT_EQUAL_INT(DeviceControlJournalState::Empty,
                          journal.state());
    TEST_ASSERT_TRUE(journal.save(expected_command, expected_result));
    TEST_ASSERT_EQUAL_INT(DeviceControlJournalState::Valid, journal.state());
  }
  NativeDeviceControlResultJournal restarted(kPath);
  TEST_ASSERT_TRUE(restarted.recover());
  DeviceControlCommand recovered_command{};
  DeviceControlResult recovered_result{};
  TEST_ASSERT_TRUE(restarted.load(recovered_command, recovered_result));
  TEST_ASSERT_EQUAL_STRING(expected_command.command_id.bytes.data(),
                           recovered_command.command_id.bytes.data());
  TEST_ASSERT_EQUAL_UINT32(expected_command.payload_size,
                           recovered_command.payload_size);
  TEST_ASSERT_EQUAL_MEMORY(expected_command.payload.data(),
                           recovered_command.payload.data(),
                           expected_command.payload_size);
  TEST_ASSERT_EQUAL_INT(expected_result.status, recovered_result.status);
  TEST_ASSERT_EQUAL_UINT32(expected_result.payload_size,
                           recovered_result.payload_size);
  TEST_ASSERT_EQUAL_MEMORY(expected_result.payload.data(),
                           recovered_result.payload.data(),
                           expected_result.payload_size);
  TEST_ASSERT_EQUAL_UINT32(expected_command.attempt, recovered_command.attempt);
  TEST_ASSERT_EQUAL_UINT64(expected_command.expires_at_us,
                           recovered_command.expires_at_us);
  TEST_ASSERT_TRUE(restarted.clear());
  TEST_ASSERT_EQUAL_INT(DeviceControlJournalState::Empty, restarted.state());
  remove_files();
}

void test_newer_corrupt_slot_falls_back_to_valid_previous_slot() {
  remove_files();
  NativeDeviceControlResultJournal journal(kPath);
  const auto first = command("cmd-a");
  const auto first_result = result("cmd-a");
  TEST_ASSERT_TRUE(journal.save(first, first_result));
  const auto second = command("cmd-b");
  const auto second_result = result("cmd-b");
  TEST_ASSERT_TRUE(journal.save(second, second_result));
  // The newest slot is .b because the first durable generation uses .a.
  std::FILE* file = std::fopen("/tmp/sigap-device-control-journal-test.b", "r+b");
  TEST_ASSERT_NOT_NULL(file);
  TEST_ASSERT_EQUAL_INT('X', std::fputc('X', file));
  TEST_ASSERT_EQUAL_INT(0, std::fclose(file));
  NativeDeviceControlResultJournal restarted(kPath);
  TEST_ASSERT_TRUE(restarted.recover());
  DeviceControlCommand recovered_command{};
  DeviceControlResult recovered_result{};
  TEST_ASSERT_TRUE(restarted.load(recovered_command, recovered_result));
  TEST_ASSERT_EQUAL_STRING("cmd-a", recovered_command.command_id.bytes.data());
  remove_files();
}

void test_all_corrupt_slots_are_not_empty_and_block_recovery() {
  remove_files();
  NativeDeviceControlResultJournal journal(kPath);
  TEST_ASSERT_TRUE(journal.save(command(), result()));
  TEST_ASSERT_TRUE(journal.save(command("cmd-2"), result("cmd-2")));
  for (const char* path : {"/tmp/sigap-device-control-journal-test.a",
                           "/tmp/sigap-device-control-journal-test.b"}) {
    std::FILE* file = std::fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT('X', std::fputc('X', file));
    TEST_ASSERT_EQUAL_INT(0, std::fclose(file));
  }
  NativeDeviceControlResultJournal restarted(kPath);
  TEST_ASSERT_FALSE(restarted.recover());
  TEST_ASSERT_EQUAL_INT(DeviceControlJournalState::Corrupt, restarted.state());
  DeviceControlCommand recovered_command{};
  DeviceControlResult recovered_result{};
  TEST_ASSERT_FALSE(restarted.load(recovered_command, recovered_result));
  TEST_ASSERT_FALSE(restarted.save(command("new"), result("new")));
  remove_files();
}

void test_clear_removes_both_slots_and_is_idempotent() {
  remove_files();
  NativeDeviceControlResultJournal journal(kPath);
  TEST_ASSERT_TRUE(journal.save(command(), result()));
  TEST_ASSERT_TRUE(journal.save(command("cmd-2"), result("cmd-2")));
  TEST_ASSERT_TRUE(journal.clear());
  TEST_ASSERT_TRUE(journal.clear());
  TEST_ASSERT_EQUAL_INT(DeviceControlJournalState::Empty, journal.state());
  remove_files();
}

}  // namespace

void setup() {}
void loop() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_save_restart_loads_exact_result);
  RUN_TEST(test_newer_corrupt_slot_falls_back_to_valid_previous_slot);
  RUN_TEST(test_all_corrupt_slots_are_not_empty_and_block_recovery);
  RUN_TEST(test_clear_removes_both_slots_and_is_idempotent);
  return UNITY_END();
}
