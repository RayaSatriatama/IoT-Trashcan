#include <unity.h>
#include <initializer_list>
#include "debug/io_gate.h"
namespace {
using namespace sigap::debug;
void test_debug_excludes_every_owner_and_wrong_release_is_safe() {
  RuntimeIoGate gate;
  TEST_ASSERT_TRUE(gate.try_acquire(IoOwner::ArchiveHttp));
  TEST_ASSERT_TRUE(gate.try_acquire(IoOwner::ControlHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::DebugHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::Filesystem));
  gate.release(IoOwner::Filesystem);
  TEST_ASSERT_TRUE(gate.active(IoOwner::ArchiveHttp));
  gate.release(IoOwner::ArchiveHttp);
  gate.release(IoOwner::ControlHttp);
  TEST_ASSERT_TRUE(gate.try_acquire(IoOwner::DebugHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::DebugHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::ArchiveHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::ControlHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::Filesystem));
  gate.release(IoOwner::DebugHttp);
  gate.release(IoOwner::DebugHttp);
  TEST_ASSERT_FALSE(gate.active(IoOwner::DebugHttp));
}
void test_filesystem_excludes_http_but_archive_and_control_coexist() {
  RuntimeIoGate gate;
  TEST_ASSERT_TRUE(gate.try_acquire(IoOwner::ArchiveHttp));
  TEST_ASSERT_TRUE(gate.try_acquire(IoOwner::ControlHttp));
  gate.release(IoOwner::ArchiveHttp);
  gate.release(IoOwner::ControlHttp);
  TEST_ASSERT_TRUE(gate.try_acquire(IoOwner::Filesystem));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::ArchiveHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::ControlHttp));
  TEST_ASSERT_FALSE(gate.try_acquire(IoOwner::DebugHttp));
}
void test_duplicate_acquisition_and_wrong_double_release_for_every_owner() {
  for (const auto owner : {IoOwner::ArchiveHttp, IoOwner::ControlHttp,
                           IoOwner::DebugHttp, IoOwner::Filesystem}) {
    RuntimeIoGate gate;
    TEST_ASSERT_TRUE(gate.try_acquire(owner));
    TEST_ASSERT_FALSE(gate.try_acquire(owner));
    gate.release(owner == IoOwner::ArchiveHttp ? IoOwner::ControlHttp : IoOwner::ArchiveHttp);
    TEST_ASSERT_TRUE(gate.active(owner));
    gate.release(owner);
    gate.release(owner);
    TEST_ASSERT_FALSE(gate.active(owner));
  }
}
}  // namespace
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_debug_excludes_every_owner_and_wrong_release_is_safe);
  RUN_TEST(test_filesystem_excludes_http_but_archive_and_control_coexist);
  RUN_TEST(test_duplicate_acquisition_and_wrong_double_release_for_every_owner);
  return UNITY_END();
}
