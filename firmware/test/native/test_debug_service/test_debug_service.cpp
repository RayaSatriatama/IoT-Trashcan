#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <unity.h>

#include "debug/service.h"
#include "debug/codec.h"
#include "debug/registry.h"
#include "debug/service.cpp"
#include "debug/codec.cpp"

namespace {
using namespace sigap;
class TestLock final : public debug::IDebugLock {
 public:
  void lock() noexcept override { ++locks; }
  void unlock() noexcept override { ++unlocks; }
  unsigned locks{};
  unsigned unlocks{};
};
acquisition::BootId boot(uint8_t tail = 1U) {
  acquisition::BootId id{};
  id.present = true;
  id.bytes[15] = tail;
  return id;
}
debug::EventInput event(uint64_t time = 1U, debug::Level level = debug::Level::Info) {
  debug::EventInput input{};
  input.monotonic_us = time;
  input.module = debug::Module::Runtime;
  input.event_code = 1U;
  input.level = level;
  return input;
}
void test_ring_overflow_reports_gap_and_oldest() {
  TestLock lock;
  debug::DebugService service(boot(), "TEST", debug::Level::Info, lock);
  for (unsigned i = 0; i < 65; ++i) TEST_ASSERT_TRUE(service.emit(event(i + 1U)));
  const auto page = service.read({boot(), 0U});
  TEST_ASSERT_TRUE(page.has_event);
  TEST_ASSERT_TRUE(page.gap);
  TEST_ASSERT_EQUAL_UINT64(2U, page.oldest);
  TEST_ASSERT_EQUAL_UINT64(1U, page.overwritten);
  TEST_ASSERT_EQUAL_UINT64(2U, page.event.sequence);
}
void test_independent_reads_capture_and_codec_boundaries() {
  TestLock lock;
  debug::DebugService service(boot(), "TEST", debug::Level::Info, lock);
  TEST_ASSERT_TRUE(service.emit(event(20U)));
  const debug::Cursor cursor{boot(), 0U};
  TEST_ASSERT_TRUE(service.read(cursor).has_event);
  TEST_ASSERT_TRUE(service.read(cursor).has_event);
  debug::BatchSnapshot snapshot{};
  TEST_ASSERT_TRUE(service.capture(cursor, 8U, 20U, snapshot));
  TEST_ASSERT_EQUAL_UINT32(1U, snapshot.count);
  TEST_ASSERT_EQUAL_UINT64(20U, snapshot.meta.captured_monotonic_us);
  char bytes[4096]{};
  size_t length = 0U;
  TEST_ASSERT_TRUE(debug::encode_meta(snapshot.meta, bytes, sizeof(bytes), length));
  TEST_ASSERT_TRUE(length > 0U);
  const auto page = service.read(cursor);
  TEST_ASSERT_TRUE(debug::encode_page(page, bytes, sizeof(bytes), length));
  TEST_ASSERT_TRUE(length > 0U);
  TEST_ASSERT_TRUE(page.event.sequence <= page.next_after);
  TEST_ASSERT_TRUE(debug::encode_upload("TEST-DEVICE", snapshot, bytes, sizeof(bytes), length));
  TEST_ASSERT_TRUE(length > 0U);
  TEST_ASSERT_FALSE(service.capture(cursor, 9U, 20U, snapshot));
  TEST_ASSERT_FALSE(service.capture(cursor, 8U, 19U, snapshot));
}
void test_invalid_filtered_presence_levels_loss_and_instance_reset() {
  TestLock lock;
  debug::DebugService service(boot(), "TEST", debug::Level::Info, lock);
  auto input = event();
  input.event_code = 999U;
  TEST_ASSERT_FALSE(service.emit(input));
  input = event(1U, debug::Level::Debug);
  TEST_ASSERT_FALSE(service.emit(input));
  TEST_ASSERT_FALSE(service.emit(event(2U, static_cast<debug::Level>(9U))));
  input = event(); input.presence = 4U;
  TEST_ASSERT_FALSE(service.emit(input));
  input = event(); input.request_id = 1U;
  TEST_ASSERT_FALSE(service.emit(input));
  TEST_ASSERT_EQUAL_UINT64(0U, service.meta(0U).latest);
  input = event(); input.presence = debug::kHasRequestId;
  TEST_ASSERT_TRUE(service.emit(input));
  TEST_ASSERT_EQUAL_UINT64(1U, service.read({boot(), 0U}).event.sequence);
  input = event(); input.presence = debug::kHasRequestId; input.request_id = 0U;
  TEST_ASSERT_TRUE(service.emit(input));
  TEST_ASSERT_TRUE(service.set_level(debug::Level::Debug));
  TEST_ASSERT_TRUE(service.emit(event(3U, debug::Level::Debug)));
  TEST_ASSERT_FALSE(service.set_level(static_cast<debug::Level>(9U)));
  service.note_delivery_loss(UINT64_MAX);
  service.note_delivery_loss(1U);
  service.note_usb_loss(UINT64_MAX);
  auto meta = service.meta(4U);
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, meta.delivery_lost);
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, meta.usb_lost);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(debug::Level::Debug), static_cast<uint8_t>(meta.level));
  TestLock second_lock;
  debug::DebugService second(boot(), "TEST", debug::Level::Info, second_lock);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(debug::Level::Info), static_cast<uint8_t>(second.meta(0U).level));
  TEST_ASSERT_EQUAL_UINT32(lock.locks, lock.unlocks);
}
void test_boot_change_empty_presence_and_sequence_exhaustion() {
  TestLock lock;
  debug::DebugService empty(boot(), "TEST", debug::Level::Info, lock);
  auto result = empty.read({boot(2U), 0U});
  TEST_ASSERT_TRUE(result.boot_changed);
  TEST_ASSERT_FALSE(result.has_event);
  result = empty.read({boot(), 0U});
  TEST_ASSERT_FALSE(result.has_event);
  TEST_ASSERT_EQUAL_UINT64(0U, result.oldest);
  acquisition::SequenceAllocator exhausted(UINT64_MAX);
  debug::DebugService final_service(boot(), "TEST", debug::Level::Info, lock, exhausted);
  TEST_ASSERT_FALSE(final_service.emit(event()));
  TEST_ASSERT_TRUE(final_service.meta(0U).sequence_exhausted);
  acquisition::BootId unavailable{};
  debug::DebugService absent(unavailable, "TEST", debug::Level::Info, lock);
  TEST_ASSERT_FALSE(absent.emit(event()));
  TEST_ASSERT_FALSE(absent.meta(0U).boot.present);
  debug::DebugService bad_build(boot(), "", debug::Level::Info, lock);
  TEST_ASSERT_FALSE(bad_build.emit(event()));
}
void test_invalid_build_bytes_fail_closed_without_records_or_codecs() {
  TestLock lock;
  const char nul_build[] = {'A', '\0', 'B'};
  const char non_ascii[] = {static_cast<char>(0x80)};
  const char too_long[97] = {};
  const char max_build[96] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                              ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                              ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                              ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                              ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                              ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                              ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                              ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '~'};
  const std::string_view invalid[] = {"BAD\nID", "BAD\tID", "BAD\rID", "BAD\x7fID",
                                      non_ascii, std::string_view(nul_build, sizeof(nul_build)),
                                      std::string_view(too_long, sizeof(too_long))};
  char bytes[4096]{};
  size_t length = 1U;
  for (const auto build : invalid) {
    debug::DebugService service(boot(), build, debug::Level::Info, lock);
    TEST_ASSERT_FALSE(service.emit(event()));
    TEST_ASSERT_FALSE(service.set_level(debug::Level::Debug));
    TEST_ASSERT_EQUAL_UINT64(0U, service.meta(0U).latest);
    debug::Meta meta = service.meta(0U);
    TEST_ASSERT_FALSE(debug::encode_meta(meta, bytes, sizeof(bytes), length));
    debug::BatchSnapshot snapshot{};
    TEST_ASSERT_FALSE(service.capture({boot(), 0U}, 8U, 1U, snapshot));
    TEST_ASSERT_FALSE(debug::encode_upload("TEST-DEVICE", snapshot, bytes,
                                           sizeof(bytes), length));
  }
  for (const std::string_view build : {std::string_view(" "),
                                       std::string_view(max_build, sizeof(max_build))}) {
    debug::DebugService service(boot(), build, debug::Level::Info, lock);
    TEST_ASSERT_TRUE(service.emit(event()));
    TEST_ASSERT_EQUAL_UINT64(1U, service.meta(1U).latest);
  }
  TEST_ASSERT_EQUAL_UINT32(lock.locks, lock.unlocks);
}
void test_page_preserves_same_boot_cursor_until_event_or_boot_change() {
  TestLock lock;
  debug::DebugService service(boot(), "TEST", debug::Level::Info, lock);
  TEST_ASSERT_TRUE(service.emit(event()));
  char bytes[4096]{};
  size_t length = 0U;
  const uint64_t cursors[] = {1U, 2U, UINT64_MAX};
  for (const uint64_t after : cursors) {
    const auto page = service.read({boot(), after});
    TEST_ASSERT_FALSE(page.has_event);
    TEST_ASSERT_EQUAL_UINT64(after, page.next_after);
    TEST_ASSERT_TRUE(debug::encode_page(page, bytes, sizeof(bytes), length));
    char expected[32]{};
    std::snprintf(expected, sizeof(expected), "|%llu|",
                  static_cast<unsigned long long>(after));
    TEST_ASSERT_NOT_NULL(std::strstr(bytes, expected));
    const auto repeated = service.read({boot(), page.next_after});
    TEST_ASSERT_FALSE(repeated.has_event);
    TEST_ASSERT_EQUAL_UINT64(after, repeated.next_after);
  }
  debug::DebugService empty(boot(), "TEST", debug::Level::Info, lock);
  const auto empty_page = empty.read({boot(), 42U});
  TEST_ASSERT_FALSE(empty_page.has_event);
  TEST_ASSERT_EQUAL_UINT64(0U, empty_page.oldest);
  TEST_ASSERT_EQUAL_UINT64(42U, empty_page.next_after);
  TEST_ASSERT_TRUE(debug::encode_page(empty_page, bytes, sizeof(bytes), length));
  TEST_ASSERT_NOT_NULL(std::strstr(bytes, "|42|"));
  const auto changed = service.read({boot(2U), 42U});
  TEST_ASSERT_TRUE(changed.boot_changed);
  TEST_ASSERT_EQUAL_UINT64(0U, changed.next_after);
  TEST_ASSERT_TRUE(service.emit(event(2U)));
  const auto resumed = service.read({boot(), 1U});
  TEST_ASSERT_TRUE(resumed.has_event);
  TEST_ASSERT_EQUAL_UINT64(2U, resumed.event.sequence);
  TEST_ASSERT_EQUAL_UINT64(2U, resumed.next_after);
  TEST_ASSERT_TRUE(resumed.event.sequence <= resumed.next_after);
}
}  // namespace
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ring_overflow_reports_gap_and_oldest);
  RUN_TEST(test_independent_reads_capture_and_codec_boundaries);
  RUN_TEST(test_invalid_filtered_presence_levels_loss_and_instance_reset);
  RUN_TEST(test_boot_change_empty_presence_and_sequence_exhaustion);
  RUN_TEST(test_invalid_build_bytes_fail_closed_without_records_or_codecs);
  RUN_TEST(test_page_preserves_same_boot_cursor_until_event_or_boot_change);
  return UNITY_END();
}
