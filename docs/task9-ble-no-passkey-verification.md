# SIGAP BLE no-passkey software verification

**Initial implementation date:** 2026-10-18
**Review correction date:** 2026-10-19
**Repository:** `feat/full-integration-hil-validation`, HEAD `0257b9e5cc08b142348f85f6da184842c09ab420`
**Scope result:** software implementation PASS; Task 9 remains HOLD; BLE security hardening is `DEFERRED_BY_OWNER`.

## 2026-10-19 correction: eFuse wire identity

The earlier review correction misread the helper's human-order MAC hex as the
firmware wire ID, so its `SIGAP-C150` target was wrong. No firmware code,
configuration, build artifact, or hardware state is changed for this correction.

The recorded hardware MAC fixture is `98:A3:16:F5:C1:50`. The installed Arduino
SDK's `ESP.getEfuseMac()` loads the eFuse bytes into a `uint64_t`; the ESP32-S3
target compiler is little-endian. Therefore the source-derived wire ID for that
fixture is `000050c1f516a398`, not the human-order MAC-hex string
`000098a316f5c150`. The diagnostic firmware formats the low 16 bits of that
raw integer, so its expected name is `SIGAP-A398`. This derives from historical
physical MAC evidence plus installed SDK/target behavior; it is not a new
physical readback.

- `scripts/ble-manual/session.ts`, the HTML target/name, fixtures, and active
  manual instructions now use wire ID `000050c1f516a398` and name
  `SIGAP-A398`. The UI labels the target as a wire ID and separates it from
  human-order hardware MAC text.
- The source-contract regression calculates the raw uint64 from the recorded
  six-byte MAC fixture and two zero high bytes in little-endian order. It checks
  the independent expected wire ID and the firmware low-16-bit name rule, so it
  fails against the former incorrect helper constants.
- Completed no-auth UI text and selected-target routing are unchanged. Device
  identity matching remains a routing check, HTTP/API authorization remains
  separate, and the web-calibration runbook retains the owner-approved
  unauthenticated BLE boundary.
- The prior bundle at
  `/home/raya/.hermes/artifacts/sigap-ble-no-passkey-review-correction-20261019-083853/`
  remains historical for its helper/report hashes. Fresh final logs and
  manifests for this identity correction are in
  `/home/raya/.hermes/artifacts/sigap-ble-no-passkey-identity-correction-20261019-021406/`.

## Final host-only closure on 2026-10-19

After renewed owner authorization, the main agent ran the final checks
individually against the corrected helper, without application-code, firmware,
or hardware changes. Results in the identity-correction bundle:

| Check | Final result |
|---|---|
| Root Python source-contract and guard suite | PASS, 9/9 tests; 11/11 NVS guard fixtures |
| Manual BLE helper tests | PASS, 10/10 |
| Full frontend suite | PASS, 130/130 tests across 23 files |
| TypeScript typecheck | PASS, exit 0 |
| Web export | PASS, exit 0; exported to `dist` |
| Git whitespace/error check | PASS, exit 0 |
| Original firmware source/configuration hashes | MATCH, 16/16 records |
| Original linked-artifact manifest comparisons | MATCH, no mismatches |

The original 450/450 native-suite result, clean cppcheck result, and seven
successful firmware builds remain retained evidence for unchanged firmware
inputs; those builds were not repeated for helper-only corrections.
`final-checks.json` records the checks, and `firmware-unchanged-verification.json`
records the source/artifact comparisons. Final source hashes and line counts
are recorded in `corrected-source-hashes.txt` and `file-line-counts.txt`.
The previously denied batch was not executed; these checks used the later
explicit owner authorization. Physical discovery, GATT, reconnect, and NVS
preservation remain NOT_RUN. No commit, push, merge, or flash was performed.

## Earlier UI correction evidence (historical for helper identity)

The prior 2026-10-19 UI correction checks below remain useful for that earlier
source state, but are not the final checks for the corrected helper. The earlier
Wi-Fi-label and manual-name RED/GREEN runs are in the previous worker transcript
at `/home/raya/.hermes/cache/delegation/live/deleg_3d34fb60/task-0.log`
(entries 77-82 and 87-92).

| Check | Earlier result |
|---|---|
| Focused BLE manual helper tests | PASS, 10/10 |
| Focused UI, runbook, manual-name and active-instruction regressions | PASS, 4/4 |
| `npm test -- --run` | PASS, 130 tests across 23 files |
| `npm run typecheck` | PASS |
| `npm run export:web` | PASS; an earlier run emitted Tamagui warning 001 for two skipped modules; final earlier rerun emitted no warning |
| Root Python suite, `python -m unittest discover -s tests -p 'test_*.py' -v` | PASS, 9 tests; NVS guard fixtures 11/11 |
| `git diff --check` | PASS |

The original firmware evidence bundle remains
`/home/raya/.hermes/artifacts/sigap-ble-no-passkey-20261018-224203/`. Current
source/configuration and retained ELF/BIN/map hash comparisons for this
correction are recorded in the new identity-correction bundle; no firmware was
rebuilt.

## Implemented behavior

- Removed the owner-authorized BLE-only source files `firmware/lib/provisioning/src/provisioning/provisioning_secret_store.h` and `.cpp`, and removed their declarations, callers, and obsolete tests. The separate application `SecretStore` for Wi-Fi credentials and device tokens remains.
- Removed setup-passkey generation/storage/restore, USB `ble passkey`, one-shot passkey handoff, enforced pairing/MITM gates, authentication callbacks, authenticated characteristic permissions, and application authentication checks from the BLE provisioning and calibration transports.
- GATT now uses ordinary command `WRITE | WRITE_NR` and response `READ | NOTIFY` properties. `GET_DEVICE_ID` remains an unauthenticated device-reported routing check, not authentication. The browser connects before that optional identity check. Status and calibration retain existing routing/session checks without a passkey.
- F1/R1 framing and bounds, response matching, session ownership, expiry, replay/idempotency controls, shared calibration ownership, profile validation, preview/commit safeguards, and safety behavior remain covered by host tests.
- Wi-Fi candidate provisioning remains bounded. The application stores credentials through its existing SecretStore only after association and IP success. AppConfig/profile mutation permissions, HTTP/API authorization, and HTTPS certificate verification remain separate and unchanged. Unauthenticated nearby BLE provisioning is a known prototype risk.
- The BLE-only diagnostic uses a dummy Wi-Fi backend and read-only session. Its application composition does not instantiate sensor, actuator, or storage services.

## NVS and NimBLE boundary

- No NVS or bond contents were read, erased, migrated, rotated, deleted, flashed, or otherwise changed during this host-only task.
- All ESP32 build environments set `CONFIG_BT_NIMBLE_MAX_BONDS=0`; the installed NimBLE-Arduino 2.5.1 configuration maps this to `MYNEWT_VAL(BLE_STORE_MAX_BONDS)`. The BLE implementation has compile-time assertions for zero bonds and `BLE_STORE_CONFIG_PERSIST=0`.
- The final diagnostic link map includes NimBLE's generic `ble_store_config` functions, but does not include `ble_store_nvs.c.o`; the final ELF has no `NvsProvisioningSecretStore`, provisioning setup-secret, or BLE passkey-command symbols. Generic NimBLE pairing/passkey APIs and `NimBLEDevice::m_passkey` remain in the upstream library image; they are not configured by the application as a passkey or persistent bond flow.
- Diagnostic wrappers reject application NVS record access and every NVS/partition erase path, including individual GC-page erase requests. The optional `phy` namespace open returns `ESP_ERR_INVALID_STATE` without latching application failure. Final SDK disassembly shows the denied PHY-record load follows the SDK's nonzero-load-result branch and skips its NVS calibration-store call. This is source/link evidence only.
- `NimBLEDevice::init()` and Arduino startup still call the real SDK `nvs_flash_init()`. SDK initialization can scan NVS metadata; host checks cannot prove physical read/write preservation or radio startup. The `phy` fallback behavior is not physically verified.

## Final verification results

The software results below were run on 2026-10-18 against the original implementation,
before the review corrections dated 2026-10-19. These results are historical
for that earlier source state and do not verify the corrections below. Firmware
source and build artifacts remained unchanged during the 2026-10-19 corrections.
Logs are in `/home/raya/.hermes/artifacts/sigap-ble-no-passkey-20261018-224203/`.

| Check | Result |
|---|---|
| Root Python suite, `python -m unittest discover -s tests -p 'test_*.py' -v` | PASS, 6 tests; NVS guard fixtures 11/11 |
| Diagnostic safety test, `python -m unittest discover -s tests -p 'test_ble_diagnostic_safety.py' -v` | PASS, 6 tests; NVS guard fixtures 11/11 |
| NimBLE restore-patch contract tests | PASS, 8 tests |
| NimBLE host sanitizer regression runner | PASS, 5/5 fixtures; patched production source SHA-256 matches the runner source SHA-256 |
| PlatformIO native BLE provisioning tests | PASS, 15/15 |
| PlatformIO native maintenance tests | PASS, 32/32 |
| Full PlatformIO native suite | PASS, 450/450 tests across 42 suites |
| PlatformIO native cppcheck | PASS, no defects |
| Focused BLE frontend tests | PASS, 19 tests across 2 files |
| `npm test -- --run` | PASS, 130 tests across 23 files |
| `npm run typecheck` | PASS |
| `npm run export:web` | PASS |
| Target builds, run serially | PASS: `sigap_ble_diag`, `sigap_n16r8`, `sigap_n16r8_sd`, `sigap_n16r8_sd_spi`, `sigap_bench`, `sigap_n16r8_https_compile`, `sigap_n16r8_sd_https_compile` |
| `git diff --check` | PASS; final command log: `git-diff-check-final.log` |

The frontend regression test first failed because the identity-change callback did not fire on ordinary GATT connect. After renaming the callback and state to verified device identity, the focused 19-test suite and full frontend suite passed. RED and GREEN logs are `frontend-no-auth-red.log` and `frontend-no-auth-green.log`.

## Build evidence and limits

The final diagnostic image was built, not flashed. SHA-256:

- `firmware.elf`: `8c5ce8eae923f1e387ca4d1a3e52cadf490eee9d82dec0576dedfef175565308`
- `firmware.bin`: `90ffc1d176bac1db4e11b2ed50188d12041018726126678d49ca4c9e07b192f5`
- `firmware.map`: `993af98fd31e5bfc9ec600d7ff3d5fb1486e68a85ebe308b9273a06736f6019e`

ELF, BIN, map, symbol/strings output, PHY disassembly, object hashes, source hashes, and per-command logs are stored in the artifact directory above. Hash manifests: `final-diag-build-hashes.txt`, `final-linked-artifact-hashes.txt`, `tracked-source-hashes-final.txt`, and `untracked-source-hashes-final.txt`.

No device access, BLE scan/chooser/pairing/connect, reset, flash, NVS/bond operation, sensor/actuator/SD operation, commit, push, or merge was performed. Host and build results do not establish physical discovery, GATT status, reconnect, NVS preservation, or Task 9 acceptance. Historical authenticated/physical reports remain historical; the commissioning ledger marks them as not active procedure. The 1,649-line `docs/task9-verification.md` was left read-only; this scoped report supersedes only its pre-change source and software-verification claims.

## 2026-10-19 intermediate `OK CHUNK` ACK correction

The firmware sends same-request `R1` `OK CHUNK` progress while an `F1` request is incomplete. It is not terminal identity, status, or calibration data. `lib/ble/protocol.ts` now recognizes only the exact single-frame `R1` progress body. Both `ManualBleSession` and `BleProvisioningClient` discard it only after matching the active request ID, before response assembly. Terminal response parsing remains strict. Progress does not clear the pending request, consume a terminal chunk index, extend its deadline, or trigger retries.

The manual and production fake-GATT peers emit repeated matching progress frames before the `writeValue` promise resolves, a mismatched-ID progress frame, and multi-frame terminal replies. Regressions cover identity, status, calibration, remaining writes, terminal assembly, fixed timeout, and disconnect cleanup. RED evidence:

- `fix-evidence/manual-ack-red.log`: focused manual identity test failed with `BLE response envelope is invalid` before the fix.
- `fix-evidence/client-ack-red.log`: focused production identity test timed out before the fix.
- `fix-evidence/client-progress-red.log`: production multi-frame status and calibration cases timed out before the fix.
- The initial immutable host replay remains at `/home/raya/.hermes/artifacts/sigap-ble-chunk-ack-20261019-134208/repro.mjs`, `served-client-before.mjs`, and `repro-red.log`. The separate `fix-evidence/repro-green.mjs` imports the fixed helper bundle; its run returned `PASS`, identity `000050c1f516a398`, `writes: 2`, and `physical_io: false`. This is synthetic host evidence only.

Final checks, each exit 0:

| Check | Result |
|---|---|
| `node_modules/.bin/vitest run lib/ble/protocol.test.ts` | 10/10 |
| `node_modules/.bin/vitest run tests/ble-manual-session.test.ts` | 12/12 |
| `node_modules/.bin/vitest run lib/ble/client.test.ts` | 12/12 |
| `npm test -- --run` | 135/135 tests, 23/23 files |
| `npm run typecheck` | PASS |
| `npm run export:web` | PASS |
| `python -m unittest discover -s tests -p 'test_*.py' -v` | 9 tests; NVS guard fixtures 11/11 |
| `git diff --check` | PASS |

Final-check logs, copied assets, and manifests are in `/home/raya/.hermes/artifacts/sigap-ble-chunk-ack-20261019-134208/fix-evidence/`; the original RED proof remains unchanged at the artifact root. The corrected helper was built with the local esbuild command and `index.html` installed into `/home/raya/.cache/sigap-ble-manual/`. HTTP GETs from the existing server at `127.0.0.1:8765` returned both assets. Downloaded and installed SHA-256 values match: `index.html` `d6c0aa5dea8b9166cb3f7f775230210682e74225f8f9c953c9fad6fac3f9c821`; `session.js` `a8c968b88252690fb74bd99aa045003a379bfc1267b247e09aa19de4bafd50d0`. Server PID `997512` remains active. An already-open operator tab was not reloaded; refresh is required before using the corrected helper.

No firmware source was edited or rebuilt, and no hardware was accessed. A fresh source-only SHA-256 comparison matched all 16 retained firmware source/config records. The reviewed diagnostic image remains the retained `firmware.bin` hash `90ffc1d176bac1db4e11b2ed50188d12041018726126678d49ca4c9e07b192f5`; this task did not reflash it. Physical BLE acceptance remains `HUMAN_ACTION_REQUIRED` and `NOT_RUN`. After refreshing the local page, the operator can manually choose and connect, verify identity, read diagnostic status, and report any remaining physical error; do not infer physical success from host tests.
