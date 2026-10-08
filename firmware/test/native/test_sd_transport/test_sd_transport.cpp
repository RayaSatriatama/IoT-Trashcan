#include <unity.h>

#include "sigap/board/sd_transport.h"

namespace {

void test_external_spi_policy_matches_sigap_contract() {
  const auto policy = sigap::board::external_spi_sd_policy();
  TEST_ASSERT_EQUAL_UINT8(39U, policy.clk);
  TEST_ASSERT_EQUAL_UINT8(40U, policy.cmd_or_miso);
  TEST_ASSERT_EQUAL_UINT8(41U, policy.d0_or_mosi);
  TEST_ASSERT_EQUAL_UINT8(43U, policy.cs);
  TEST_ASSERT_FALSE(policy.mode_1bit);
  TEST_ASSERT_TRUE(sigap::board::sd_transport_policy_valid(policy));
}

void test_onboard_sd_mmc_policy_is_explicit_and_non_formatting() {
  const auto policy = sigap::board::sd_mmc_1bit_policy();
  TEST_ASSERT_EQUAL_UINT8(39U, policy.clk);
  TEST_ASSERT_EQUAL_UINT8(38U, policy.cmd_or_miso);
  TEST_ASSERT_EQUAL_UINT8(40U, policy.d0_or_mosi);
  TEST_ASSERT_TRUE(policy.mode_1bit);
  TEST_ASSERT_FALSE(policy.format_if_mount_failed);
  TEST_ASSERT_TRUE(sigap::board::sd_transport_policy_valid(policy));
}

void test_gpio38_is_not_allowed_by_external_spi_policy() {
  auto policy = sigap::board::external_spi_sd_policy();
  policy.cmd_or_miso = 38U;
  TEST_ASSERT_TRUE(sigap::board::sd_transport_conflicts_with_board(policy));
  TEST_ASSERT_FALSE(sigap::board::sd_transport_policy_valid(policy));
}

void test_default_build_keeps_external_spi_selected() {
#ifndef SIGAP_SD_TRANSPORT_SDMMC_1BIT
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::board::SdTransport::ExternalSpi),
      static_cast<int>(sigap::board::kSdTransportPolicy.transport));
  TEST_ASSERT_EQUAL_STRING("external_spi",
                          sigap::board::sd_transport_name(
                              sigap::board::kSdTransportPolicy.transport));
#else
  TEST_IGNORE_MESSAGE("SD_MMC policy is an explicit target build selection");
#endif
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_external_spi_policy_matches_sigap_contract);
  RUN_TEST(test_onboard_sd_mmc_policy_is_explicit_and_non_formatting);
  RUN_TEST(test_gpio38_is_not_allowed_by_external_spi_policy);
  RUN_TEST(test_default_build_keeps_external_spi_selected);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
