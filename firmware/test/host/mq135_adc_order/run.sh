#!/usr/bin/env bash
# Build and run the S1 B1 MQ-135 ADC ordering regression on the host.
#
# The firmware headers under test are the real target adapters. Only the SDK
# surface is stubbed (include/Arduino.h, include/esp_adc_cal.h, include/Wire.h,
# include/esp_timer.h, include/freertos/portmacro.h). No PlatformIO target is
# touched, no hardware, no dependency is added, and the binary is written
# outside the repository.
#
# Usage: firmware/test/host/mq135_adc_order/run.sh [output-binary]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
firmware="$(cd "${here}/../../.." && pwd)"
out="${1:-${TMPDIR:-/tmp}/sigap_mq135_adc_order/test_mq135_adc_order}"

mkdir -p "$(dirname "${out}")"

includes=()
for lib in config acquisition maintenance calibration provisioning processing \
           models fuzzy safety storage telemetry_archive telemetry_contract \
           network debug; do
  includes+=("-I${firmware}/lib/${lib}/src")
done

${CXX:-g++} -std=c++17 -O1 -DARDUINO -Wall -Wextra -Werror -Wno-unused-parameter \
  -I"${here}/include" "-I${firmware}/include" "${includes[@]}" \
  "${here}/test_mq135_adc_order.cpp" -o "${out}"

"${out}"
