#include <cstring>
#include <string>
#include <string_view>
#include "provisioning/ble_service_queue.h"
#include "unity.h"

using namespace sigap::provisioning;
namespace {
bool retains_secret(const BleServiceQueue& queue) {
  const std::string_view bytes(reinterpret_cast<const char*>(&queue),sizeof(queue));
  return bytes.find("TEST_QUEUE_SECRET")!=bytes.npos;
}
void test_payload_is_owned_and_consumed_storage_is_scrubbed() {
  BleServiceQueue queue;queue.connect(7,123);BleServiceEvent event;
  TEST_ASSERT_TRUE(queue.pop(event));TEST_ASSERT_EQUAL_UINT64(123,event.received_us);
  std::string source="TEST_QUEUE_SECRET";TEST_ASSERT_TRUE(queue.write(7,source,124));source.assign("changed");
  TEST_ASSERT_TRUE(queue.pop(event));TEST_ASSERT_EQUAL_STRING_LEN("TEST_QUEUE_SECRET",event.payload.data(),17);
  TEST_ASSERT_FALSE(retains_secret(queue));TEST_ASSERT_TRUE(queue.matches(event));
}
void test_overflow_scrubs_every_slot_and_reports_failure_once() {
  BleServiceQueue queue;queue.connect(7,0);BleServiceEvent event;queue.pop(event);
  for(int i=0;i<16;++i)TEST_ASSERT_TRUE(queue.write(7,"TEST_QUEUE_SECRET",0));
  TEST_ASSERT_FALSE(queue.write(7,"one too many",0));TEST_ASSERT_FALSE(queue.pop(event));
  TEST_ASSERT_FALSE(retains_secret(queue));TEST_ASSERT_TRUE(queue.consume_failure());TEST_ASSERT_FALSE(queue.consume_failure());
  const auto connection=queue.connection();TEST_ASSERT_TRUE(connection.failed);TEST_ASSERT_TRUE(queue.terminate(connection));TEST_ASSERT_FALSE(queue.terminate(connection));
  TEST_ASSERT_FALSE(queue.write(7,"PING",0));
}
void test_reconnect_invalidates_epoch_even_when_handle_is_reused() {
  BleServiceQueue queue;queue.connect(7,0);BleServiceEvent old;queue.pop(old);
  TEST_ASSERT_TRUE(queue.write(7,"TEST_QUEUE_SECRET",0));queue.connect(7,1);
  TEST_ASSERT_FALSE(queue.matches(old));TEST_ASSERT_FALSE(retains_secret(queue));BleServiceEvent current;TEST_ASSERT_TRUE(queue.pop(current));
  TEST_ASSERT_TRUE(current.epoch!=old.epoch);TEST_ASSERT_TRUE(queue.matches(current));TEST_ASSERT_FALSE(queue.pop(current));
}
void test_foreign_handle_cannot_mutate_current_connection() {
  BleServiceQueue queue;queue.connect(7,0);BleServiceEvent event;queue.pop(event);
  TEST_ASSERT_FALSE(queue.write(8,"PING",0));TEST_ASSERT_FALSE(queue.disconnect(8,1));TEST_ASSERT_TRUE(queue.connection().connected);
  TEST_ASSERT_TRUE(queue.write(7,std::string(192,'X'),1));TEST_ASSERT_TRUE(queue.pop(event));TEST_ASSERT_EQUAL_UINT16(192,event.size);
  TEST_ASSERT_FALSE(queue.write(7,std::string(193,'X'),1));TEST_ASSERT_TRUE(queue.consume_failure());
}
void test_disconnect_scrubs_pending_payload_and_marks_old_writes_stale() {
  BleServiceQueue queue;queue.connect(7,0);BleServiceEvent event;queue.pop(event);queue.write(7,"TEST_QUEUE_SECRET",0);queue.pop(event);
  queue.write(7,"TEST_QUEUE_SECRET",0);TEST_ASSERT_TRUE(queue.disconnect(7,1));TEST_ASSERT_FALSE(queue.matches(event));TEST_ASSERT_FALSE(retains_secret(queue));
  TEST_ASSERT_TRUE(queue.pop(event));TEST_ASSERT_TRUE(event.kind==BleServiceEventKind::Disconnect);TEST_ASSERT_TRUE(queue.matches(event));TEST_ASSERT_FALSE(queue.connection().connected);
}
void test_old_termination_cannot_disconnect_new_connection() {
  BleServiceQueue queue;queue.connect(7,0);const auto old=queue.connection();queue.connect(8,1);
  TEST_ASSERT_FALSE(queue.terminate(old));TEST_ASSERT_TRUE(queue.connection().connected);TEST_ASSERT_FALSE(queue.connection().failed);
}
}
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_payload_is_owned_and_consumed_storage_is_scrubbed);
  RUN_TEST(test_overflow_scrubs_every_slot_and_reports_failure_once);
  RUN_TEST(test_reconnect_invalidates_epoch_even_when_handle_is_reused);
  RUN_TEST(test_foreign_handle_cannot_mutate_current_connection);
  RUN_TEST(test_disconnect_scrubs_pending_payload_and_marks_old_writes_stale);
  RUN_TEST(test_old_termination_cannot_disconnect_new_connection);
  return UNITY_END();
}
