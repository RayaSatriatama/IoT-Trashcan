# Task 9 BLE-only diagnostic software evidence (2026-10-18)

> Historical report for the pre-change authenticated BLE image. Its source snapshots and image hashes do not represent the current no-passkey source. Physical observations remain preserved as recorded. Current host-only evidence is in [task9-ble-no-passkey-verification.md](task9-ble-no-passkey-verification.md).

Physical follow-up under a fresh scoped owner decision is recorded separately
in [task9-ble-physical-verification.md](task9-ble-physical-verification.md).
App-only flash/readback and bounded boot passed; authenticated GATT remains
blocked by disconnect during service discovery. The NOT_RUN gates below record
the pre-upload software snapshot, not the later physical run.

## Historical strict no-erase candidate

Current bundle:
`/home/raya/.hermes/artifacts/sigap-ble-strict-no-erase-20261018-095147/`.
Branch remains `feat/full-integration-hil-validation`, HEAD
`0257b9e5cc08b142348f85f6da184842c09ab420`; changes remain uncommitted.

The lead compared all 24 old snapshotted source files and found no worktree
drift, then identified a no-erase scope defect: the old guard allowed a single
aligned NVS page erase. The minimal follow-up changed the existing diagnostic
guard and regression test only. A new requirement was tested first: reject all
five aligned pages in the synthetic NVS partition and call the real erase
function zero times. The old guard compiled but the harness exited 12, recorded
as expected RED in `no-erase-red.log`. The strict guard then passed.

The current partition-erase wrapper unconditionally latches failure and returns
`ESP_ERR_INVALID_STATE`. The GC exception and real erase reference were removed.
If NVS needs a page erase, the diagnostic must stop; pairing can fail when free
NVS space is insufficient. There is no erase/retry fallback. The change applies
only to the diagnostic image, excluded from production target source selection.

Fresh checks: diagnostic fixtures 2/2, NVS error scenarios 10/10, dependency
contracts 8/8, native BLE 17/17, cppcheck with no defects, and all seven firmware
environments listed in the historical command table below rebuilt serially
with exit 0. Commands/logs are in the current bundle's `checks.json`.
The actual NimBLE sanitizer reproduction was also rerun: upstream `empty`,
`our-only`, and `peer-only` failed with negative-index diagnostics as expected;
patched source passed all five fixtures. Scratch patching and repeat
idempotency passed, with no installed dependency-source mutation.

Final disassembly shows the erase wrapper calls only `latch_failure()` and
returns `0x103`. Direct callers `initArduino`, `rewrite_ota_seq$part$1`, and
`nvs::NVSPartition::erase_range` resolve to that wrapper; no direct call to the
real partition erase function was found. Checked production app/Wi-Fi/SD/
LittleFS symbols remain absent. This is source/fixture/linked-image evidence,
not a complete indirect-call proof, physical boot proof or zero-write promise.
See `linked-image-review.json` in the current bundle.

| Current file | Bytes | SHA-256 |
| --- | ---: | --- |
| `firmware.bin` | 496080 | `b7913a74db474be9e56691d98a0634132bce2cb6a8481efedf24b389e2375630` |
| `firmware.elf` | 12298452 | `aee227119c5d5d415044aa2bad758d88e6a7fbd855b1239fe3576bdb0ca45078` |
| `firmware.map` | 12032002 | `6806fd4bad3b4eb42787323df91f6a6c32e099c2b76ebe9a76f37693e8674a77` |

The current image is ESP32-S3, 16 MB, 80 MHz, DIO, with valid checksum/hash.
Its app offset is `0x10000`, image end `0x891d0`, and sector-rounded write end
`0x8a000`, inside the factory partition ending at `0x410000`. The new
`app-only-manifest.json` includes only the app image. Normal application-sector
erase during an authorized flash is distinct from forbidden runtime NVS erase.
The old bundle and its different binary hashes remain historical only.

Rejecting erase does not make NVS immutable: NimBLE may create its RW
`nimble_bond` namespace, and pairing/CCCD persistence can write without an erase.
These startup/pairing writes require current scoped permission before physical
use. Missing/invalid setup secret, erase request, NVS error, full bond store,
bond replacement/deletion, identity mismatch or unexpected subsystem activity
must stop the run. No board/serial/BLE, sensor/actuator, SD, flash/reset, secret
readout, commit, push or merge occurred in this follow-up. The old physical NVS
mismatch remains historical FAIL. The oversized main ledger was not expanded.

```text
BLE_DIAGNOSTIC_SOFTWARE=PASS
NVS_ERASE_GUARD=PASS_HOST_AND_LINKED_IMAGE
BLE_DIAGNOSTIC_IMAGE=SOFTWARE_REVIEW_PASS_PHYSICAL_AUTHORIZATION_REQUIRED
NVS_AUTHORIZATION=BLOCKED_PENDING_CURRENT_OWNER_DECISION
NVS_PRESERVED=HISTORICAL_FAIL
APP_FLASH=NOT_RUN
BLE_MITM_AUTHENTICATED=NOT_RUN
REAL_GET_DEVICE_ID=NOT_RUN
REAL_GET_STATUS=NOT_RUN
BLE_RECONNECT=NOT_RUN
TASK9_STATUS=HOLD
```

## Historical permissive candidate

All remaining sections describe the earlier candidate and its original hashes,
not the current strict no-erase candidate. Current results and decisions above
take precedence; the old bundle is preserved unchanged.

### Scope and provenance

- Repository branch `feat/full-integration-hil-validation`, HEAD
  `0257b9e5cc08b142348f85f6da184842c09ab420`; changes remain uncommitted.
- This is a host-only continuation from the interrupted Luna 6 (xhigh) handoff.
  The owner renewed permission for the previously denied host-only artifact
  checks and later requested continuation after a separate host-only patch
  reproduction was denied. The lead then ran a separate scratch-only fixture
  against the pinned input and recorded its exact output and source hashes in
  the bundle. No installed dependency source was changed by this fixture.
- Bundle (outside the repository):
  `/home/raya/.hermes/artifacts/sigap-ble-diagnostic-20261018-044753/`.
  Its `source/` snapshot and `source-manifest.json` capture the exact worktree
  inputs reviewed here; the binary, ELF, map, test logs and
  `app-only-manifest.json` are separate files. Generated binaries remain
  ignored by Git in the normal PlatformIO build directory.
- No board, USB/serial device, BLE controller, NVS partition, sensor, actuator,
  SD card or physical flash was accessed. No secret or passkey was disclosed.
  No commit, push or merge.

## Host-only verification on the final source snapshot

Workdir is the repository root except where stated. Every command below
returned exit 0 unless its expected RED outcome is stated explicitly.

| Check | Exact invocation | Observed |
| --- | --- | --- |
| Diagnostic NVS/secret fixtures | `python -m unittest discover -s tests -p test_ble_diagnostic_safety.py -v` | 2/2 tests and 10/10 NVS guard scenarios passed. |
| Dependency contracts | From `firmware/`: `SIGAP_NIMBLE_ROOT="$PWD/.pio/libdeps/sigap_ble_diag/NimBLE-Arduino" python -m unittest test/host/test_nimble_restore_patch.py -v` | 8/8 passed; source drift/version rejection, idempotency and diagnostic repeat-pairing rejection checked. |
| BLE protocol/session | From `firmware/`: `/home/raya/.local/bin/pio test -e native -f native/test_ble_provisioning` | 17/17 cases passed, including diagnostic read-only command rejection and diagnostic status. |
| Static | From `firmware/`: `/home/raya/.local/bin/pio check -e native --fail-on-defect=low` | cppcheck reported no defects, 1 tool succeeded. |
| RED/GREEN actual C store path | Bundle `tests/test_dependency_reproduction.py` using the reviewed upstream and patched hashes with `firmware/test/host/nimble_restore/run.py` | Unpatched source: 5 fixtures, 2 pass and 3 expected sanitizer failures with negative index on `empty`, `our-only`, `peer-only`. Scratch clone transforms were applied and repeated idempotently. Installed patched source: 5/5 pass. |
| Target compilation | From `firmware/`: `/home/raya/.local/bin/pio run -e <env>`, sequentially for `sigap_ble_diag`, `sigap_n16r8`, `sigap_n16r8_sd`, `sigap_n16r8_sd_spi`, `sigap_bench`, `sigap_n16r8_https_compile`, and `sigap_n16r8_sd_https_compile` | All 7/7 exit 0. See `build-*.log` and `checks.json` in the bundle. |
| Diff whitespace | `git diff --check` | Exit 0. |

The local NimBLE-Arduino dependency is still version `2.5.1`. The build hook
applies the hash-guarded zero-bond fix, SHA-256
`b5f7d79dcbdc568977f1f93dfe12f7edd046ce832f2eeab8056e4ad81b366bd3`.
The diagnostic environment alone applies the additional upstream-server
repeat-pairing reject patch, SHA-256
`da2bcef89c0f18d913a31cd401d02412a096e0e0797c26f2842ae3487c99a65e`.
The other six builds logged `repeat-pairing diagnostic patch=not-applied`.
This is a local patch, not a new upstream Arduino library version; downgrade
or upgrade remains unverified as a fix for this defect.

## Reviewed image, not a physical PASS

`esptool.py v4.11.0 image_info --version 2` identified ESP32-S3,
16 MB, 80 MHz, image header DIO, valid checksum and validation hash.
The same host tool identified DIO in the production `sigap_n16r8` image,
although the PlatformIO board settings specify QIO; this is an observed
header/config difference, not a tested device failure. Review exact target
flash/PSRAM identity before physical use. The factory application partition
is `0x10000..0x410000` (exclusive end), and the diagnostic image ends at
`0x89290`. The 4 KiB-sector-rounded region would end at `0x8a000`, inside
that app partition. The manifest lists only this application file and address.

| File copied into bundle | Bytes | SHA-256 |
| --- | ---: | --- |
| `firmware.bin` | 496272 | `0703bd336c9130aad58975474007545f8a6911a24e0aed7f886d08955b8da993` |
| `firmware.elf` | 12299780 | `323fd0c9357654a911caa3f13c9d5f882473150df553aff68282b5dbb85d1c05` |
| `firmware.map` | 12032610 | `60eb7dfbe972e6314fe1694311c31aeff3e62dd397531fafcf8897e4e49ffa05` |

`xtensa-esp32s3-elf-nm -C firmware.elf` and `firmware.map` were inspected.
Linked symbols contain the diagnostic BLE service, fail latch and in-use
NVS/partition erase wrappers; no production `SigapApp`, `esp_wifi_init`,
`esp_wifi_start`, `esp_wifi_connect`, SD/LittleFS mount or associated production
objects were found. Some wrapper functions defined in source, such as
`__wrap_nvs_erase_all`, are absent from the ELF because no linked caller
uses them. Symbol absence alone cannot prove every side effect impossible.
No physical boot or readback has verified startup on the real board.

The diagnostic source checks a pre-existing exact-size setup secret through a
read-only NVS handle and exposes only authenticated GET_DEVICE_ID, GET_STATUS
and PING with explicit diagnostic status. Other Wi-Fi and calibration commands
are rejected. Local maintenance enter/one-shot passkey handoff uses the existing
production path and is not a BLE secret getter. This is test/source evidence,
not a physical assertion about the board's current NVS contents.

## Remaining gates and stop rules

NimBLE may open `nimble_bond` read-write at startup and create the namespace;
pairing and CCCD persistence also write NVS. The wrapper blocks whole-partition
NVS erase, but allows an aligned single 4 KiB NVS page erase for GC. That
permission does not establish that an allowed erase was necessary GC, nor prove
byte-for-byte NVS preservation. The old physical NVS mismatch remains
`NVS_PRESERVED=FAIL`; this host bundle neither repairs nor reclassifies it.
The image must not be flashed before the lead reviews the manifest, exact
startup paths, target identity, and owner decisions on existing-secret reuse
and necessary BLE persistence. Any invalid/missing secret, full bond store,
NVS error, unexpected deletion/replacement, or mismatched flash/partition
identity is a stop condition. The manifest's flash/readback commands are
nonexecuted templates, not authorization. Further physical flash/reset and
pairing require the applicable explicit owner permission.

```text
BLE_DIAGNOSTIC_SOFTWARE=PASS (host fixtures, static check, seven builds)
BLE_DIAGNOSTIC_IMAGE=REVIEW_REQUIRED
NVS_AUTHORIZATION=BLOCKED_PENDING_OWNER_REVIEW
NVS_PRESERVED=HISTORICAL_FAIL
APP_FLASH=NOT_RUN
BLE_MITM_AUTHENTICATED=NOT_RUN
REAL_GET_DEVICE_ID=NOT_RUN
REAL_GET_STATUS=NOT_RUN
BLE_RECONNECT=NOT_RUN
TASK9_STATUS=HOLD
```
