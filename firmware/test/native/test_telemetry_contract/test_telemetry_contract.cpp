#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <unity.h>

#include "telemetry/contract.h"

namespace {

using sigap::telemetry_contract::Uint64Decimal;
using sigap::telemetry_contract::is_canonical_uint64;
using sigap::telemetry_contract::record_fixture_hash;

void test_uint64_wire_values_are_canonical() {
  TEST_ASSERT_TRUE(is_canonical_uint64("0"));
  TEST_ASSERT_TRUE(is_canonical_uint64("18446744073709551615"));
  TEST_ASSERT_FALSE(is_canonical_uint64("00"));
  TEST_ASSERT_FALSE(is_canonical_uint64("01"));
  TEST_ASSERT_FALSE(is_canonical_uint64("-1"));
  TEST_ASSERT_FALSE(is_canonical_uint64("18446744073709551616"));
}

void test_uint64_formatter_is_bounded_and_exact() {
  Uint64Decimal value{};
  TEST_ASSERT_TRUE(value.assign(UINT64_MAX));
  constexpr char expected[] = "18446744073709551615";
  TEST_ASSERT_EQUAL_UINT32(sizeof(expected) - 1U, value.size);
  TEST_ASSERT_EQUAL_INT(0, std::memcmp(value.bytes.data(), expected, value.size));
}

void test_canonical_record_fixture_matches_go_golden_digest() {
  constexpr std::string_view canonical{
      "{\"boot_id\":\"boot-A\",\"config_ref\":{\"config_hash\":\"sha256:0000000000000000000000000000000000000000000000000000000000000000\",\"config_version\":\"4\",\"schema_version\":1},\"converted\":null,\"device_id\":\"SIGAP-01\",\"kind\":\"sample\",\"monotonic_us\":\"123\",\"profile_refs\":[],\"quality\":\"UNCALIBRATED\",\"raw\":{\"adc_counts\":1234},\"reason\":\"PROFILE_MISSING\",\"record_id\":\"SIGAP-01/boot-A/1\",\"recorded_at\":null,\"sensor_id\":\"mq4\",\"sequence\":\"1\",\"stream\":\"organic\",\"time_quality\":\"UNSYNCED\"}"};
  constexpr std::array<uint8_t, 32> expected{
      0xa3U, 0xa2U, 0x59U, 0x36U, 0x18U, 0x94U, 0x3cU, 0xc7U,
      0xa1U, 0xd5U, 0xf8U, 0xbcU, 0x0cU, 0x90U, 0x21U, 0x6dU,
      0xc2U, 0x2eU, 0xb4U, 0xc3U, 0x29U, 0x7fU, 0xb8U, 0x74U,
      0x88U, 0x7cU, 0x11U, 0xb7U, 0xb6U, 0xefU, 0x68U, 0x83U};
  const auto actual = record_fixture_hash(canonical);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), actual.data(), expected.size());
}

void test_exact_body_fixture_matches_go_body_hash() {
  constexpr std::string_view body{R"SIGAP({"schema_version":3,"device_id":"SIGAP-01","batch_id":"SIGAP-01/boot-A/batch-1","record_count":1,"samples":[{"record_id":"SIGAP-01/boot-A/1","record_hash":"sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883","kind":"sample","device_id":"SIGAP-01","boot_id":"boot-A","sequence":"1","monotonic_us":"123","recorded_at":null,"time_quality":"UNSYNCED","config_ref":{"schema_version":1,"config_version":"4","config_hash":"sha256:0000000000000000000000000000000000000000000000000000000000000000"},"profile_refs":[],"sensor_id":"mq4","stream":"organic","quality":"UNCALIBRATED","reason":"PROFILE_MISSING","raw":{"adc_counts":1234},"converted":null}],"results":[],"events":[],"metadata":[]})SIGAP" "\n"};
  constexpr std::array<uint8_t, 32> expected{
      0x36U, 0x20U, 0xedU, 0xf0U, 0x54U, 0x8bU, 0x8aU, 0x15U,
      0xebU, 0x16U, 0x38U, 0xfcU, 0x7dU, 0x23U, 0x9bU, 0x19U,
      0x95U, 0x37U, 0x2aU, 0x28U, 0x98U, 0x2fU, 0x7fU, 0x1eU,
      0x19U, 0x49U, 0x49U, 0x16U, 0x0fU, 0x16U, 0x6cU, 0x7cU};
  const auto actual = record_fixture_hash(body);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), actual.data(), expected.size());
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_uint64_wire_values_are_canonical);
  RUN_TEST(test_uint64_formatter_is_bounded_and_exact);
  RUN_TEST(test_canonical_record_fixture_matches_go_golden_digest);
  RUN_TEST(test_exact_body_fixture_matches_go_body_hash);
  UNITY_END();
}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
