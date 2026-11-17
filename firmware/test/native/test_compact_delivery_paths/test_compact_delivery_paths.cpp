#define SIGAP_FRESH_WEB_PRIORITY14 1
#include <unity.h>
#include <cstring>
#include <cstdio>
#include "storage/live_delivery_paths.h"

using sigap::acquisition::BootId;
using sigap::storage::live_delivery::BootPaths;

namespace {
BootId boot_id(const char* hex) {
  BootId id{};
  id.present = true;
  for (size_t i = 0; i < 16U; ++i) {
    unsigned value{};
    std::sscanf(hex + 2U * i, "%2x", &value);
    id.bytes[i] = static_cast<uint8_t>(value);
  }
  return id;
}
void test_lossless_compact_name_vector_and_last_byte() {
  const BootPaths vector(boot_id("000102030405060708090a0b0c0d0e0f"));
  const BootPaths changed(boot_id("000102030405060708090a0b0c0d0e0e"));
  TEST_ASSERT_TRUE(vector.valid);
  TEST_ASSERT_EQUAL_STRING("/sigap/AAECAwQFBgcICQoLDA0ODw.s", vector.archive.data());
  TEST_ASSERT_NOT_EQUAL(0, std::strcmp(vector.archive.data(), changed.archive.data()));
}
void test_names_distinct_and_filesystem_component_bounded() {
  const BootPaths paths(boot_id("000102030405060708090a0b0c0d0e0f"));
  const char* values[]{paths.archive.data(), paths.checkpoint.data(), paths.gaps.data(), paths.batches.data(), paths.batch_checkpoint.data(), paths.acks.data(), paths.ack_checkpoint.data()};
  const char* ext[]{".s", ".c", ".g", ".b", ".d", ".a", ".k"};
  for (size_t i = 0; i < 7U; ++i) {
    TEST_ASSERT_EQUAL_UINT(24U, std::strlen(values[i]) - 6U);
    TEST_ASSERT_TRUE(std::strstr(values[i], ext[i]) != nullptr);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(32U, std::strlen(values[i]) - 6U + 8U);
    for (size_t j = i + 1U; j < 7U; ++j) TEST_ASSERT_NOT_EQUAL(0, std::strcmp(values[i], values[j]));
  }
  TEST_ASSERT_LESS_OR_EQUAL_UINT(32U, 22U + 7U);
  TEST_ASSERT_LESS_OR_EQUAL_UINT(32U, 22U + 6U);
}
void test_missing_zero_and_owned_strings() {
  BootId absent{}; TEST_ASSERT_FALSE(BootPaths(absent).valid);
  BootId zero{}; zero.present = true; TEST_ASSERT_FALSE(BootPaths(zero).valid);
  BootId input = boot_id("000102030405060708090a0b0c0d0e0f");
  const BootPaths paths(input); char saved[BootPaths::kPathSize];
  std::strcpy(saved, paths.archive.data()); input.bytes.fill(0U);
  TEST_ASSERT_EQUAL_STRING(saved, paths.archive.data());
}
}
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_lossless_compact_name_vector_and_last_byte);
  RUN_TEST(test_names_distinct_and_filesystem_component_bounded);
  RUN_TEST(test_missing_zero_and_owned_strings);
  return UNITY_END();
}
