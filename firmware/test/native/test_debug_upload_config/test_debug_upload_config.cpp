#include <unity.h>
#include <string>
#include "sigap/build/build_config.h"

void test_debug_upload_config_policy() {
  const auto valid = sigap::build::DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/TEST-DEVICE/events",
      "TEST-DEVICE");
  TEST_ASSERT_TRUE(valid.available());
  TEST_ASSERT_FALSE(sigap::build::DebugUploadConfig::make("", "TEST-DEVICE").available());
  TEST_ASSERT_FALSE(sigap::build::DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/TEST-DEVICE/events", "").available());
  TEST_ASSERT_FALSE(sigap::build::DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/OTHER/events", "TEST-DEVICE").available());
  TEST_ASSERT_FALSE(sigap::build::DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/events/TEST-DEVICE", "TEST-DEVICE").available());
  TEST_ASSERT_FALSE(sigap::build::DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/TEST-DEVICE/events ", "TEST-DEVICE").available());
}

void test_debug_endpoint_rejects_query_fragment_and_wrong_root() {
  using sigap::build::DebugUploadConfig;
  TEST_ASSERT_TRUE(DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/TEST/events", "TEST").available());
  for (const auto url : {
      "https://example.invalid?x=/api/device-debug/device/TEST/events",
      "https://example.invalid#/api/device-debug/device/TEST/events",
      "https://example.invalid/wrong/api/device-debug/device/TEST/events",
      "https://user:password@example.invalid/api/device-debug/device/TEST/events"}) {
    TEST_ASSERT_FALSE(DebugUploadConfig::make(url, "TEST").available());
  }
}

void test_debug_config_preserves_exact_utf8_and_encoded_path_identity() {
  using sigap::build::DebugUploadConfig;
  TEST_ASSERT_TRUE(DebugUploadConfig::make(
      u8"https://example.invalid/api/device-debug/device/TEST-ñ/events", u8"TEST-ñ").available());
  TEST_ASSERT_TRUE(DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/TEST-%C3%B1/events", u8"TEST-ñ").available());
  TEST_ASSERT_TRUE(DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/A%2FB%3FC%23D/events", "A/B?C#D").available());
  TEST_ASSERT_FALSE(DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/A/B/events", "A/B").available());
  TEST_ASSERT_FALSE(DebugUploadConfig::make(
      "https://example.invalid/api/device-debug/device/TEST-%GG/events", "TEST-%GG").available());
}

void test_debug_config_byte_bounds_and_invalid_utf8() {
  using sigap::build::DebugUploadConfig;
  const std::string code(64U, 'a');
  const auto url = std::string("https://x/api/device-debug/device/") + code + "/events";
  TEST_ASSERT_TRUE(DebugUploadConfig::make(url, code).available());
  TEST_ASSERT_FALSE(DebugUploadConfig::make(url + "a", code + "a").available());
  TEST_ASSERT_FALSE(DebugUploadConfig::make(
      std::string("https://") + std::string(192U, 'a') + "/api/device-debug/device/TEST/events", "TEST").available());
  const std::string invalid(1U, static_cast<char>(0xff));
  TEST_ASSERT_FALSE(DebugUploadConfig::make(
      "https://x/api/device-debug/device/" + invalid + "/events", invalid).available());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_debug_upload_config_policy);
  RUN_TEST(test_debug_endpoint_rejects_query_fragment_and_wrong_root);
  RUN_TEST(test_debug_config_preserves_exact_utf8_and_encoded_path_identity);
  RUN_TEST(test_debug_config_byte_bounds_and_invalid_utf8);
  return UNITY_END();
}