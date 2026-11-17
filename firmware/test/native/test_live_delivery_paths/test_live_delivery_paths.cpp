#define SIGAP_FRESH_WEB_PRIORITY14 1
#include <unity.h>
#include <cstring>
#include "storage/live_delivery_paths.h"

using sigap::acquisition::BootId;
using sigap::storage::live_delivery::BootPaths;

namespace {
BootId boot_id(uint8_t last = 1U) {
  BootId id{};
  id.present = true;
  id.bytes[0] = 0x12U;
  id.bytes[15] = last;
  return id;
}

void test_paths_are_distinct_and_bounded() {
  const BootPaths paths(boot_id());
  TEST_ASSERT_TRUE(paths.valid);
  const char* values[]{paths.archive.data(), paths.checkpoint.data(), paths.gaps.data(),
      paths.batches.data(), paths.batch_checkpoint.data(), paths.acks.data(),
      paths.ack_checkpoint.data()};
  for (size_t i = 0; i < 7U; ++i) {
    TEST_ASSERT_LESS_THAN(BootPaths::kPathSize, std::strlen(values[i]) + 1U);
    for (size_t j = i + 1U; j < 7U; ++j) TEST_ASSERT_NOT_EQUAL(0, std::strcmp(values[i], values[j]));
  }
}

void test_paths_are_deterministic_and_boot_specific() {
  const BootPaths first(boot_id(1U));
  const BootPaths same(boot_id(1U));
  const BootPaths other(boot_id(2U));
  TEST_ASSERT_EQUAL_STRING(first.archive.data(), same.archive.data());
  TEST_ASSERT_NOT_EQUAL(0, std::strcmp(first.archive.data(), other.archive.data()));
}

void test_paths_own_strings_after_input_changes() {
  BootId input = boot_id();
  const BootPaths paths(input);
  char saved[BootPaths::kPathSize];
  std::strcpy(saved, paths.archive.data());
  input.bytes.fill(0U);
  TEST_ASSERT_EQUAL_STRING(saved, paths.archive.data());
}

void test_missing_or_zero_boot_id_is_rejected() {
  BootId absent{};
  const BootPaths missing(absent);
  TEST_ASSERT_FALSE(missing.valid);
  BootId zero{};
  zero.present = true;
  const BootPaths all_zero(zero);
  TEST_ASSERT_FALSE(all_zero.valid);
}
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_paths_are_distinct_and_bounded);
  RUN_TEST(test_paths_are_deterministic_and_boot_specific);
  RUN_TEST(test_paths_own_strings_after_input_changes);
  RUN_TEST(test_missing_or_zero_boot_id_is_rejected);
  return UNITY_END();
}
