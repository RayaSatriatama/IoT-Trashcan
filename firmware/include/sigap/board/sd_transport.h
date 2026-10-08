#pragma once

#include <cstdint>

#include "sigap/board/board_pins.h"

namespace sigap::board {

enum class SdTransport : uint8_t {
  ExternalSpi = 0U,
  SdMmc1Bit = 1U,
};

struct SdTransportPolicy {
  SdTransport transport{SdTransport::ExternalSpi};
  uint8_t clk{0U};
  uint8_t cmd_or_miso{0U};
  uint8_t d0_or_mosi{0U};
  uint8_t cs{0U};
  bool mode_1bit{false};
  bool format_if_mount_failed{false};
  uint32_t spi_frequency_hz{20'000'000U};
  const char* mount_point{"/sdcard"};
  uint8_t max_files{5U};
};

constexpr SdTransportPolicy external_spi_sd_policy() noexcept {
  SdTransportPolicy policy{};
  policy.transport = SdTransport::ExternalSpi;
  policy.clk = 39U;
  policy.cmd_or_miso = 40U;
  policy.d0_or_mosi = 41U;
  policy.cs = 43U;
  policy.spi_frequency_hz = 20'000'000U;
  return policy;
}

constexpr SdTransportPolicy sd_mmc_1bit_policy() noexcept {
  SdTransportPolicy policy{};
  policy.transport = SdTransport::SdMmc1Bit;
  policy.clk = 39U;
  policy.cmd_or_miso = 38U;
  policy.d0_or_mosi = 40U;
  policy.mode_1bit = true;
  policy.format_if_mount_failed = false;
  policy.mount_point = "/sdcard";
  policy.max_files = 5U;
  return policy;
}

#ifdef SIGAP_SD_TRANSPORT_SDMMC_1BIT
inline constexpr SdTransportPolicy kSdTransportPolicy{sd_mmc_1bit_policy()};
#else
inline constexpr SdTransportPolicy kSdTransportPolicy{external_spi_sd_policy()};
#endif

constexpr bool sd_pin_matches_sensor(uint8_t pin, const BoardPins& pins) {
  const auto values = pin_values(pins);
  // cppcheck-suppress useStlAlgorithm
  for (const uint8_t value : values) {
    if (pin == value) {
      return true;
    }
  }
  return false;
}

constexpr bool sd_transport_conflicts_with_sensors(
    const BoardPins& pins,
    const SdTransportPolicy& policy) {
  if (sd_pin_matches_sensor(policy.clk, pins) ||
      sd_pin_matches_sensor(policy.cmd_or_miso, pins) ||
      sd_pin_matches_sensor(policy.d0_or_mosi, pins)) {
    return true;
  }

  if (policy.transport == SdTransport::ExternalSpi &&
      sd_pin_matches_sensor(policy.cs, pins)) {
    return true;
  }

  return false;
}

constexpr bool sd_transport_conflicts_with_board(
    const SdTransportPolicy& policy) {
  const auto pin_allowed = [&policy](uint8_t pin) constexpr {
    // GPIO38 is reserved by the generic N16R8 safety map. It is permitted
    // only by the explicit, opt-in onboard SD_MMC policy.
    return !is_reserved_pin(pin) ||
           (policy.transport == SdTransport::SdMmc1Bit && pin == 38U);
  };
  if (!pin_allowed(policy.clk) ||
      !pin_allowed(policy.cmd_or_miso) ||
      !pin_allowed(policy.d0_or_mosi)) {
    return true;
  }
  return policy.transport == SdTransport::ExternalSpi &&
         !pin_allowed(policy.cs);
}

constexpr bool sd_transport_policy_valid(
    const SdTransportPolicy& policy) {
  if (policy.format_if_mount_failed ||
      sd_transport_conflicts_with_sensors(kN16R8Pins, policy) ||
      sd_transport_conflicts_with_board(policy)) {
    return false;
  }
  if (policy.transport == SdTransport::ExternalSpi) {
    return policy.clk == 39U && policy.cmd_or_miso == 40U &&
           policy.d0_or_mosi == 41U && policy.cs == 43U &&
           !policy.mode_1bit && policy.spi_frequency_hz > 0U;
  }
  return policy.clk == 39U && policy.cmd_or_miso == 38U &&
         policy.d0_or_mosi == 40U && policy.cs == 0U && policy.mode_1bit;
}

constexpr const char* sd_transport_name(SdTransport transport) {
  return transport == SdTransport::SdMmc1Bit ? "sd_mmc_1bit" : "external_spi";
}

static_assert(!sd_transport_conflicts_with_sensors(kN16R8Pins,
                                                   kSdTransportPolicy),
              "SIGAP SD transport pins must not overlap sensor pins");
static_assert(sd_transport_policy_valid(kSdTransportPolicy),
              "selected SIGAP SD transport policy is invalid");
static_assert(sd_transport_policy_valid(external_spi_sd_policy()),
              "SIGAP external SPI SD policy is invalid");
static_assert(sd_transport_policy_valid(sd_mmc_1bit_policy()),
              "SIGAP onboard SD_MMC policy is invalid");

}  // namespace sigap::board
