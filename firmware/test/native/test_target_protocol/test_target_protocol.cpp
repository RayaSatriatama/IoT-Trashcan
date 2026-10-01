#include <array>
#include <cstdint>

#include <unity.h>

#include "acquisition/target_protocol.h"

namespace {

using namespace sigap::acquisition;

void test_ds18b20_known_scratchpad_decodes_verified_85c() {
  const std::array<uint8_t, 9> scratchpad{
      0x50U, 0x05U, 0x4bU, 0x46U, 0x7fU, 0xffU, 0x0cU, 0x10U, 0x1cU};
  int32_t temperature_centi_c{0};

  TEST_ASSERT_TRUE(decode_ds18b20_scratchpad(scratchpad, temperature_centi_c));
  TEST_ASSERT_EQUAL_INT32(8500, temperature_centi_c);
}

void test_ds18b20_bad_scratchpad_crc_is_rejected() {
  const std::array<uint8_t, 9> scratchpad{
      0x50U, 0x05U, 0x4bU, 0x46U, 0x7fU, 0xffU, 0x02U, 0x10U, 0x00U};
  int32_t temperature_centi_c{0};

  TEST_ASSERT_FALSE(decode_ds18b20_scratchpad(scratchpad, temperature_centi_c));
}

void test_sht31_known_frame_checks_both_crc_bytes() {
  // 0x6666 = 25.00 C and 0x8000 = 50.00 percent RH.
  const std::array<uint8_t, 6> frame{
      0x66U, 0x66U, 0x93U, 0x80U, 0x00U, 0xa2U};
  const auto decoded = decode_sht31_frame(frame);

  TEST_ASSERT_TRUE(decoded.temperature_crc_valid);
  TEST_ASSERT_TRUE(decoded.humidity_crc_valid);
  TEST_ASSERT_EQUAL_INT32(2500, decoded.temperature_centi_c);
  TEST_ASSERT_EQUAL_INT32(5000, decoded.humidity_centi_pct);
}

void test_sht31_bad_crc_is_tracked_per_field() {
  const std::array<uint8_t, 6> frame{
      0x66U, 0x66U, 0x00U, 0x80U, 0x00U, 0xa2U};
  const auto decoded = decode_sht31_frame(frame);

  TEST_ASSERT_FALSE(decoded.temperature_crc_valid);
  TEST_ASSERT_TRUE(decoded.humidity_crc_valid);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ds18b20_known_scratchpad_decodes_verified_85c);
  RUN_TEST(test_ds18b20_bad_scratchpad_crc_is_rejected);
  RUN_TEST(test_sht31_known_frame_checks_both_crc_bytes);
  RUN_TEST(test_sht31_bad_crc_is_tracked_per_field);
  UNITY_END();
}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
