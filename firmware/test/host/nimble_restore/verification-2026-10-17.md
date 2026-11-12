# NimBLE zero-bond restore verification — 2026-10-17

## Scope and identity

- Repository: `/mnt/nvme0n1p4/Projects/Programming_Projects/full-integration-hil-validation`
- Branch: `feat/full-integration-hil-validation`
- Start and end HEAD: `0257b9e5cc08b142348f85f6da184842c09ab420` (no commit)
- Authorization: host-only regression, native/static checks, and local builds; no board/NVS access, flashing, reset, pairing, or physical mutation.
- Firmware dependency identity: NimBLE-Arduino `2.5.1`; upstream NVS source SHA-256 `e4d61d3b6403e263d40f2f498c4d6ac64ee1727ccd3fd6f040ba0e1908c26144`; SIGAP-patched source SHA-256 `b5f7d79dcbdc568977f1f93dfe12f7edd046ce832f2eeab8056e4ad81b366bd3`.
- Patch remains a narrow local guard for each side's zero-count restore indexing. It does not change the pinned upstream version or relax passkey/MITM, encrypted/authenticated GATT, one-bond, or two-CCCD policy.

## Results

| Check | Result | Observed evidence |
| --- | --- | --- |
| Patch contract tests | **PASS** | `python -m unittest test/host/test_nimble_restore_patch.py -v`; 6 tests passed, including transform idempotency, source drift rejection, installed-library patch/repeat, and wrong-version rejection. |
| Upstream source RED control | **EXPECTED RED** | Actual upstream file compiled successfully with ASan+UBSan (`COMPILE_EXIT=0`). 5 fixtures: `empty=fail`, `both=pass`, `our-only=fail`, `peer-only=fail`, `read-error=pass`; summary 2 passed / 3 failed, runner exit 1 as expected. All three failures report runtime index `-1` bounds errors. Source hash matched the reviewed upstream hash above. |
| Installed patched source GREEN | **PASS** | Actual installed NimBLE `ble_store_nvs.c` compiled with ASan+UBSan; 5/5 fixtures passed. The host harness asserted restored own/peer counters (`7`/`11` where present), zero counts where absent, correct read-error behavior, and `write_calls == 0` in every fixture. Source hash matched the reviewed patched hash above. |
| Native BLE provisioning test | **PASS** | `/home/raya/.local/bin/pio test -e native -f native/test_ble_provisioning`; 15/15 cases succeeded. |
| Native static analysis | **PASS** | `/home/raya/.local/bin/pio check -e native --fail-on-defect=low`; cppcheck reported no defects, 1/1 tool succeeded. |
| Fresh isolated dependency-resolution/hook/build | **PASS** | Used new scratch-only `PLATFORMIO_LIBDEPS_DIR` and `PLATFORMIO_BUILD_DIR`; clean NimBLE-Arduino 2.5.1 install was hooked from upstream input to patched hash, then `sigap_n16r8` built successfully (55.26 s). Isolated metadata/source hash verified after build. |
| Six requested local builds | **PASS** | All six returned exit 0. Each environment's installed package metadata was programmatically verified as `NimBLE-Arduino/2.5.1`, and each source SHA-256 matched the patched hash above. |
| `git diff --check` | **PASS** | Final check had no whitespace errors. |

### Six build identities

| Environment | Result | Duration | Installed NimBLE identity |
| --- | --- | ---: | --- |
| `sigap_n16r8` | PASS | 15.295 s | 2.5.1 / `b5f7d79dcbdc568977f1f93dfe12f7edd046ce832f2eeab8056e4ad81b366bd3` |
| `sigap_n16r8_sd` | PASS | 14.760 s | 2.5.1 / same patched hash |
| `sigap_n16r8_https_compile` | PASS | 15.271 s | 2.5.1 / same patched hash |
| `sigap_n16r8_sd_https_compile` | PASS | 13.125 s | 2.5.1 / same patched hash |
| `sigap_n16r8_sd_spi` | PASS | 12.488 s | 2.5.1 / same patched hash |
| `sigap_bench` | PASS | 13.053 s | 2.5.1 / same patched hash |

## Limits and remaining gates

These are host/source and compile-time results only. The harness includes the actual NimBLE C restore source but replaces NVS/OS calls with host shims; it did not touch device NVS or prove runtime/byte-for-byte NVS preservation. No physical BLE pairing, MITM/authenticated GATT read, device identity/status read, reconnect, or hardware acceptance was tested. Do not mark AC31–AC34 or physical gates PASS from these results; Task 9 remains HOLD under the current verification ledger.

## Reproduction and logs

All command output, exact exit markers, and per-command records are under:

`/home/raya/.hermes/cache/scratch/sigap-ble-authorized-verification-2026-10-17/`

Key files:

- `patch-contract-tests.log` / `.exit`
- `upstream-red.log` / `.exit`
- `patched-green.log` / `.exit`
- `native-ble-test.log` / `.exit`
- `native-static-check.log` / `.exit`
- `isolated-clean.log` / `.exit`; isolated dependency at `isolated-clean/libdeps/sigap_n16r8/NimBLE-Arduino/`
- `build-sigap_n16r8.log` / `.exit`
- `build-sigap_n16r8_sd.log` / `.exit`
- `build-sigap_n16r8_https_compile.log` / `.exit`
- `build-sigap_n16r8_sd_https_compile.log` / `.exit`
- `build-sigap_n16r8_sd_spi.log` / `.exit`
- `build-sigap_bench.log` / `.exit`

No implementation or test correction was needed after these checks. This report is the only repository artifact added by this verification continuation; no previously modified tracked files were changed, and the existing worktree changes were preserved. No commit, push, or merge was made.
