#!/usr/bin/env bash
# Replays the same ordering regression against the FROZEN pre-fix snapshots
# recorded in BEFORE.json, so the RED result is reproducible from the same test
# binary that passes on the corrected tree. It proves the case is behavioural:
# only the three pre-fix headers change, the test source is identical.
#
# The shadow tree is built outside the repository; the product tree is never
# modified. Exit 0 means "RED reproduced as documented" (the test binary failed);
# exit 3 means the pre-fix headers unexpectedly passed, which would invalidate
# the regression.
#
# Usage: firmware/test/host/mq135_adc_order/replay_before.sh [output-binary]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
firmware="$(cd "${here}/../../.." && pwd)"
before_json="${SIGAP_B1_BEFORE_JSON:-/home/raya/workspace/sigap-completion-20261112/winsen-nh3-implementation/s1-b1-fix/BEFORE.json}"
work="${TMPDIR:-/tmp}/sigap_mq135_adc_order_before"
shadow="${work}/include"
out="${1:-${work}/test_mq135_adc_order_before}"

if [[ ! -f "${before_json}" ]]; then
  echo "replay_before: missing BEFORE.json at ${before_json}" >&2
  exit 2
fi

rm -rf "${shadow}"
mkdir -p "${shadow}/acquisition"

python3 - "${before_json}" "${shadow}" <<'PY'
import json, os, shutil, sys

before_json, shadow = sys.argv[1], sys.argv[2]
entries = json.load(open(before_json))
wanted = {
    "firmware/lib/acquisition/src/acquisition/mq_adc_calibration.h",
    "firmware/lib/acquisition/src/acquisition/mq_adc_calibration_esp32.h",
    "firmware/lib/acquisition/src/acquisition/arduino_adapters.h",
    "firmware/lib/acquisition/src/acquisition/target_runtime.h",
}
restored = []
for entry in entries:
    if entry["path"] not in wanted:
        continue
    dst = os.path.join(shadow, "acquisition", os.path.basename(entry["path"]))
    shutil.copyfile(entry["snapshot"], dst)
    restored.append(os.path.basename(entry["path"]))
missing = sorted(wanted - {f"firmware/lib/acquisition/src/acquisition/{n}" for n in restored})
if missing:
    sys.exit(f"replay_before: BEFORE.json is missing {missing}")
print("replay_before: restored pre-fix headers -> " + ", ".join(sorted(restored)))
PY

includes=()
for lib in config acquisition maintenance calibration provisioning processing \
           models fuzzy safety storage telemetry_archive telemetry_contract \
           network debug; do
  includes+=("-I${firmware}/lib/${lib}/src")
done

${CXX:-g++} -std=c++17 -O1 -DARDUINO -Wall -Wextra -Werror -Wno-unused-parameter \
  -I"${shadow}" -I"${here}/include" "-I${firmware}/include" "${includes[@]}" \
  "${here}/test_mq135_adc_order.cpp" -o "${out}"

set +e
"${out}"
rc=$?
set -e
echo "replay_before: pre-fix binary exit=${rc}"
if [[ "${rc}" -eq 0 ]]; then
  echo "replay_before: UNEXPECTED PASS on pre-fix headers - the regression is not behavioural" >&2
  exit 3
fi
echo "replay_before: RED reproduced against the frozen pre-fix headers"
