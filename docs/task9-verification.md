# Task 9 verification ledger

## Physical LittleFS initialization (`4df9888`, 2026-10-16)

The authorized LittleFS-only recovery was executed after a fresh read-only
preflight. The board identity was ESP32-S3 revision v0.2, MAC
`98:a3:16:f5:c1:50`, USB-Serial/JTAG, with 16 MiB flash and 8 MiB PSRAM.
`esptool.py v4.11.0 get_security_info` reported secure boot disabled and flash
encryption disabled.

The installed table was read from `0x8000` and matched
`firmware/partitions_n16r8.csv` exactly:

```text
nvs       0x00009000 0x00005000
phy_init  0x0000e000 0x00001000
factory   0x00010000 0x00400000
spiffs    0x00410000 0x00800000
```

The fresh pre-write dump at `0x410000`, size `0x800000`, was 8,388,608 bytes,
all `0xFF`, and matched the previously recorded backup hash
`9f9b02f5ee6cbef5e018c1ee424095fc21a842ea6968c0d36114b5930dab2ba1`.
A byte-identical copy was retained outside the repository at
`/tmp/sigap-littlefs-backup-before-init-4df9888.bin`. Pre-write preservation
hashes were captured for the partition table, NVS, PHY data, and factory app.

An empty filesystem image was generated with PlatformIO's `mklittlefs` 2.3
using block size 4096, page size 256, and size 8,388,608 bytes. The image hash
was `a65d2210306c433023a5be96b22392e03b673ae61471f1676f3f727487c7eade`.
The repository `buildfs` target could not run because no `firmware/data`
directory exists; the direct PlatformIO-provided tool produced and listed a
valid empty image offline without credentials or production files.

The only write was:

```text
esptool.py --chip esp32s3 --port <USB-by-id> --baud 921600 \
  --before default_reset --after no_reset write_flash \
  --flash_mode keep --flash_freq keep --flash_size keep --verify \
  0x410000 /tmp/sigap-lfs-preflight.AVUMV5/littlefs-empty.bin
```

esptool reported erase range `0x00410000` through `0x00c0ffff`, wrote exactly
8,388,608 bytes, and verified the data. A full post-write readback matched the
image hash. `verify_flash` also matched the pre-write partition table, NVS,
PHY data, and complete factory app. No SD operation was performed.

After one controlled hard reset, the boot log reported the expected board
memory and contained no `Corrupted dir pair`, `Mounting LittleFS failed`, or
`LittleFS begin() failed` message. A post-boot 8 MiB readback remained readable
by `mklittlefs` with the configured geometry, confirming a mountable physical
filesystem. The installed application is the non-HTTPS build, confirmed by the
absence of the HTTPS endpoint and device-control journal path in the app image.
Its compile guard therefore does not initialize the production
`FsDeviceControlResultJournal`; physical journal persistence and replay could
not be exercised without an application upload and runtime credentials.

```text
LITTLEFS_BACKUP: PASS
PARTITION_LAYOUT: PASS
LITTLEFS_IMAGE: PASS
LITTLEFS_WRITE: PASS
LITTLEFS_MOUNT: PASS (boot evidence, no mount error)
LITTLEFS_INTEGRITY: PASS (readback and offline listing)
DEVICE_CONTROL_JOURNAL: BLOCKED (installed app is non-HTTPS)
NVS_PRESERVATION: PASS
SD_PRESERVATION: PASS (no SD access or mutation)
SOFTWARE_REGRESSION: PASS (450/450 native, 6/6 target builds, Cppcheck)
```

The P0 LittleFS filesystem recovery is operational. The Task 9 journal
acceptance remains blocked until a separately authorized HTTPS-enabled image
can be installed and its journal recovery exercised.

## Final pushed source (`9f80ee9`, 2026-10-10)

The identity-gate implementation and its focused tests were committed and
pushed to `feat/full-integration-hil-validation`. PR #27 remains OPEN and
unmerged. GitHub reports Vercel Preview Comments SUCCESS and the deployment
context PENDING at ledger update time. The worktree is clean after the push.

The final source verification includes the fresh results below; historical
counts elsewhere in this ledger remain labelled as superseded snapshots.

## P0 remediation follow-up (`273478a` source, 2026-10-10)

The current source changes enforce authenticated BLE device identity before
any Wi-Fi status, scan, retry, or credential mutation. `WifiSettingsDialog`
now receives the provisioned canonical `ble_device_id`, performs
`GET_DEVICE_ID` verification before reading status, clears diagnostic state
on a new connection attempt, and disables mutations when identity is absent,
mismatched, or disconnected. The BLE client rejects all Wi-Fi operations
until identity verification succeeds. Focused Vitest coverage is 14/14.

The physical LittleFS partition was inspected read-only before any repair
attempt. The partition table at `0x8000` matches `firmware/partitions_n16r8.csv`
(`spiffs`, `0x410000`, `0x800000`). A complete 8 MiB dump was captured to an
operator-controlled temporary file and hashed as
`9f9b02f5ee6cbef5e018c1ee424095fc21a842ea6968c0d36114b5930dab2ba1`.
All sampled blocks, including the first and last blocks, were `0xFF` with
zero non-FF bytes. This is consistent with a blank/unformatted or otherwise
unavailable filesystem and not a single recoverable directory entry. No
format, erase, repair, NVS mutation, SD write, or journal write was attempted.
The firmware continues to mount with `LittleFS.begin(false)` and disables
device-control polling when the journal cannot recover; this is the required
fail-closed behavior. Physical LittleFS repair and real HTTPS durability
remain BLOCKED pending an owner-approved preservation and recovery operation.

Fresh source checks after this follow-up:

```text
npm test -- --run lib/ble/client.test.ts lib/ble/task9_integration.test.ts PASS 14/14
npm test -- --run                                      PASS 21 files / 115 tests
npm run typecheck                                       PASS
npm run export:web                                      PASS
```

## Lead orchestrator verification (`318f70f`, 2026-10-10)

The source verification began from `318f70f81f125a88f3a37f7e6b5bfde0f4c7cd13`.
PR #27 remains OPEN and unmerged. The follow-up documentation-only commit is
`8ced31f31cdffad4c8d360607f5a26b93cd8f202`; it contains no production-source
change. Vercel Preview Comments is successful for the follow-up commit, while
the new Vercel deployment was still pending when this ledger was updated.

Fresh host-side results:

```text
pio test -e native                                      PASS 450/450
pio check -e native --fail-on-defect=low                PASS, no defects
pio run -e sigap_n16r8                                  PASS (RAM 49,496; flash 1,106,945)
pio run -e sigap_n16r8_sd                               PASS (RAM 50,036; flash 1,180,189)
pio run -e sigap_n16r8_sd_spi                           PASS (RAM 49,792; flash 1,159,709)
pio run -e sigap_bench                                  PASS (RAM 49,496; flash 1,106,945)
pio run -e sigap_n16r8_https_compile                    PASS (RAM 51,824; flash 1,305,225)
pio run -e sigap_n16r8_sd_https_compile                 PASS (RAM 52,260; flash 1,363,341)
npm test -- --run                                      PASS (21 files, 114 tests)
npm run typecheck                                       PASS
npm run export:web                                      PASS
git diff --check                                        PASS
go test -count=1 ./...                                 PASS
go test -race -count=1 ./...                           PASS
go vet ./...                                           PASS
go test -count=1 -v ./internal/telemetry                PASS (14 schemas, 24 valid, 36 invalid, frozen hashes)
```

Go verification used an isolated official Go 1.27.1 toolchain installed under
`/tmp` after checksum validation (`63d339f0da5ab53635a56f2490a7984dfe12dfcff22ad749f63edaf590168445`).
The system Go installation and repository environment were not modified.

The combined SD+HTTPS compile-only image from this run is
`firmware/.pio/build/sigap_n16r8_sd_https_compile/firmware.bin` with SHA-256
`4b7bdd91d94a18f0ed12f63a98af88955f13a496f1895ef34ee9576880ced8fe`.
The PlatformIO board descriptor still labels the generic board as N8/no PSRAM,
while the project overrides 16 MiB flash and OPI PSRAM. This metadata mismatch
must be checked before any upload; it is not physical runtime proof.

Physical read-only observations:

```text
USB VID:PID       303a:1001 Espressif USB JTAG/serial debug unit
USB serial        98:A3:16:F5:C1:50 -> /dev/ttyACM0
ESP32-S3          revision v0.2, USB-Serial/JTAG, embedded PSRAM 8 MiB
BLE               98:A3:16:F5:C1:51, SIGAP-A398, service discovered
BlueZ             Paired=yes, Bonded=yes, Trusted=yes, Connected=no
```

`esptool chip_id` was used only for identification and caused an RTS hard
reset while opening the USB transport. No flash write, erase, NVS change, SD
write, or firmware upload occurred. PlatformIO Monitor accepted
`maintenance enter` with `OK`; the passkey command was not executed by the
agent and no passkey was captured.

The supplied serial boot evidence contains:

```text
esp_littlefs: Corrupted dir pair at {0x0, 0x1}
mount failed, (-84)
Failed to initialize LittleFS
```

This remains `LITTLEFS_RUNTIME: FAIL / UNRESOLVED`. No format, erase, repair,
or overwrite was attempted. Because the production device-control result
journal depends on LittleFS recovery, real HTTPS device-control durability is
not a physical PASS. Real WiFi, HTTPS, SD writes, BLE MITM pairing,
`GET_DEVICE_ID`, runtime high-water metrics, and long-duration endurance
remain `NOT RUN` or `HUMAN_ACTION_REQUIRED`.

## Real BLE scope-override attempt (`0590a08`)

The attached board was discovered with the host Bluetooth adapter after a
temporary, reversible adapter unblock. The USB identity was
`98:A3:16:F5:C1:50`; BlueZ discovered BLE address `98:A3:16:F5:C1:51` with
advertised SIGAP service UUID `7d3f0001-7d3f-4a53-9a4b-534947415000` and name
`SIGAP-A398`. Chromium 153 exposed a real `navigator.bluetooth` object on the
local HTTPS origin with the experimental Web Bluetooth feature enabled.

The rendered website requested the real device chooser without replacing
`navigator.bluetooth`. No device was selected during the 120-second window,
so no authenticated pairing, GATT command, `GET_STATUS`, `GET_DEVICE_ID`,
notification exchange, or reconnect occurred. The serial maintenance path was
also unavailable to the current user because `/dev/ttyACM0` is owned by group
`uucp`; the one-shot passkey was not disclosed or logged. The host adapter was
powered off and re-blocked after the attempt.

```text
REAL_BLE_DISCOVERY: PASS
REAL_BLE_PAIRING: HUMAN_ACTION_REQUIRED / NOT RUN
REAL_BLE_GATT: NOT RUN
REAL_BLE_DEVICE_ID: NOT RUN (source contract exists; runtime response unverified)
REAL_BLE_BROWSER_E2E: INCOMPLETE
HUMAN_PAIRING_STEP: select SIGAP-A398 and enter the one-shot USB-disclosed passkey
```

## Current final-source verification (`a91418a`)

This is the current evidence for the open Task 9 branch. Earlier sections are
historical snapshots and are not substitutes for this run.

```text
pio test -e native --without-uploading                         PASS 450/450
isolated ASan/UBSan pio test -e native                         PASS 450/450, no diagnostics
pio check -e native --fail-on-defect=low                       PASS, no defects
pio run -e sigap_n16r8 -e sigap_n16r8_sd -e sigap_n16r8_sd_spi \
        -e sigap_bench -e sigap_n16r8_https_compile \
        -e sigap_n16r8_sd_https_compile                       PASS, 6/6
go test -count=1 ./...                                        PASS
go test -race -count=1 ./...                                  PASS
go vet ./...                                                  PASS
go test -count=1 -v ./internal/telemetry                      PASS, 14 schemas / 24 valid / 36 invalid / frozen hashes
npm test -- --run                                            PASS, 21 files / 114 tests
npm run typecheck                                             PASS
npm run export:web                                            PASS
git diff --check                                              PASS
```

The Chromium browser harness is a synthetic GATT test, not physical pairing:

```text
SIGAP_E2E_URL=https://127.0.0.1:4173 \
SIGAP_PLAYWRIGHT_CORE=/tmp/.../playwright-core/index.js \
SIGAP_CHROMIUM=/usr/bin/chromium node scripts/task9-browser-e2e.mjs
PASS: missing mapping, verified identity, diagnostics, START, criteria,
      capture, validate, preview, identity-bound COMMIT, disconnect,
      wrong-device rejection
```

The fresh runner used Chromium `153.0.8010.52`, Node `v24.15.0`, and an
isolated `/tmp` Playwright Core installation. No Playwright package or
private key was added to the repository.

The Go loopback calibration journey is an isolated TLS/SQLite software test
and passed all six lifecycle stages. It does not prove ESP32-originated HTTPS.
The browser run uses only ephemeral test certificates and synthetic evidence;
no credentials are persisted or committed.

Final resource measurements are RAM/flash bytes:
`sigap_n16r8` 49,496/1,106,945; `sigap_n16r8_sd` 50,036/1,180,189;
`sigap_n16r8_sd_spi` 49,792/1,159,709; `sigap_bench` 49,496/1,106,945;
`sigap_n16r8_https_compile` 51,824/1,305,225; and
`sigap_n16r8_sd_https_compile` 52,260/1,363,341.
The final combined image is
`firmware/.pio/build/sigap_n16r8_sd_https_compile/firmware.bin` with SHA-256
`4b7bdd91d94a18f0ed12f63a98af88955f13a496f1895ef34ee9576880ced8fe`.

Physical ESP32 upload/reboot, WiFi association, HTTPS transmission, BLE
pairing, SD writes, LittleFS diagnosis, runtime stack/heap high-water, and
24/72-hour endurance were not run in this batch. The supplied WiFi credentials
were not used. Historical attached-only SD evidence remains separate and does
not establish the current image's physical runtime behavior.

## P0 closure verification (`d5d0169` plus identity mapping)

The production composition now binds `FsDeviceControlResultJournal` to the
HTTPS-enabled `DeviceControlPoller`. LittleFS is mounted without formatting,
the two-slot versioned/CRC journal is recovered before command fetch, and
save, load, corrupt, and clear failures remain fail-closed. Native persistence
and poller recovery regressions cover exact result replay, reboot-relative
deadlines, duplicate delivery, and unresolved journal state.

BLE identity is authenticated over encrypted MITM GATT with `GET_DEVICE_ID`.
The firmware publishes the EFUSE identity as canonical lower-case 16-hex, and
the browser clears verification on disconnect or device switch. Backend device
records now carry an optional `ble_device_id` provisioned from that EFUSE value
(`migrations/007_device_ble_identity.sql`). The website never derives BLE
identity from an advertising name or a legacy `device_code`; absent mapping is
shown as `BLE belum diprovision` and mutating BLE operations are unavailable.

Fresh source verification:

```text
pio test -e native --without-uploading                         PASS 447/447
isolated ASan/UBSan pio test -e native                         PASS 447/447, no diagnostics
pio check -e native --fail-on-defect=low                       PASS, no defects
pio run -e sigap_n16r8 -e sigap_n16r8_sd -e sigap_n16r8_sd_spi \
        -e sigap_bench -e sigap_n16r8_https_compile \
        -e sigap_n16r8_sd_https_compile                       PASS, 6/6
go test -count=1 ./...                                        PASS
go test -race -count=1 ./...                                  PASS
go vet ./...                                                  PASS
npm test -- --run                                            PASS 21 files / 114 tests
npm run typecheck                                             PASS
npm run export:web                                            PASS
git diff --check                                              PASS
```

The final native and isolated ASan/UBSan reruns after adding the ARDUINO
filesystem-double journal regressions were both 447/447 with no diagnostics.

Final target resource values are RAM/flash bytes:
`sigap_n16r8` 49,496/1,106,945; `sigap_n16r8_sd` 50,036/1,180,189;
`sigap_n16r8_sd_spi` 49,792/1,159,709; `sigap_bench` 49,496/1,106,945;
`sigap_n16r8_https_compile` 51,824/1,305,225; and
`sigap_n16r8_sd_https_compile` 52,260/1,363,341. The final combined image
SHA-256 is `4b7bdd91d94a18f0ed12f63a98af88955f13a496f1895ef34ee9576880ced8fe`.

These are software and compile/runtime-double results. Physical upload,
WiFi association, HTTPS staging, BLE pairing, SD writes, and runtime stack or
heap high-water capture were not performed in this verification pass.

## Fresh integrated Wave 1 evidence (`76c3203` source -> current builds)

## Latest final-source verification (`14a27fd`)

The final integrated source was verified after the backend, storage, browser,
and poller remediation commits. These results supersede earlier numeric totals
in this ledger while preserving the historical sections below.

```text
pio test -e native --without-uploading                         PASS 438/438
isolated ASan/UBSan pio test -e native                         PASS 438/438, no diagnostics
pio check -e native --fail-on-defect=low                       PASS, no defects
pio run -e sigap_n16r8 -e sigap_n16r8_sd                       PASS
pio run -e sigap_n16r8_sd_spi -e sigap_bench                   PASS
pio run -e sigap_n16r8_https_compile                           PASS
pio run -e sigap_n16r8_sd_https_compile                        PASS
go test -count=1 ./...                                         PASS
go test -race -count=1 ./...                                   PASS
go vet ./...                                                   PASS
go test -count=1 -v ./internal/telemetry                      PASS 14 schemas / 24 valid / 36 invalid / golden hashes
npm test -- --run                                             PASS 21 files / 112 tests
npm run typecheck                                              PASS
npm run export:web                                             PASS
git diff --check                                               PASS
```

Final target resource measurements are RAM/flash: `sigap_n16r8` 49,496 /
1,106,673; `sigap_n16r8_sd` 50,036 / 1,179,933;
`sigap_n16r8_sd_spi` 49,792 / 1,159,453; `sigap_bench` 49,496 / 1,106,673;
`sigap_n16r8_https_compile` 51,824 / 1,300,825; and
`sigap_n16r8_sd_https_compile` 52,260 / 1,358,973 bytes. The combined
SD+HTTPS image SHA-256 is
`64f28c0b7120abfe09c5cdb61e8e145f46f2b2fa43a7563827ea9ab7969a0696`.

## Historical durability remediation snapshot (`4d1f2ee` -> integrated source, superseded)

Fresh scoped fixes now include backend late-result reconciliation
(`cd9acc8`), empty-archive forensic preservation (`6b2028a`), serialized
BLE requests (`3051eef`), and fail-closed poller handling for journal save/
clear failures plus reboot-relative expiry (`14a27fd`). The focused poller
regression is 13/13 PASS, target adapter storage regressions are 16/16 PASS,
backend device-control/E2E tests pass in normal and race modes, and frontend
tests are 112/112 PASS.

At this historical snapshot the production persistent result journal and BLE
identity binding were still open. Those statements are superseded by the P0
closure section above, which records the production binding and fresh evidence.

This section supersedes stale hardware-boundary statements below while
preserving historical ledgers. Source verification was rerun on the current
working tree. The selected SD image was uploaded only to the application
range `0x10000..0x130fff`; NVS, partition metadata, and the SD card were
preserved.

Fresh lead verification on the integrated Wave 1 source:

```text
pio test -e native --without-uploading                         PASS 434/434
pio check -e native --fail-on-defect=low                        PASS, no defects
pio run -e sigap_n16r8                                          PASS RAM 49,496; flash 1,106,633
pio run -e sigap_n16r8_sd                                       PASS RAM 50,036; flash 1,179,893
pio run -e sigap_n16r8_sd_spi                                   PASS RAM 49,792; flash 1,159,409
pio run -e sigap_bench                                          PASS RAM 49,496; flash 1,106,633
pio run -e sigap_n16r8_https_compile                            PASS RAM 51,824; flash 1,300,573
pio run -e sigap_n16r8_sd_https_compile                        PASS RAM 52,260; flash 1,358,761
go test -count=1 ./...                                          PASS
go test -race -count=1 ./...                                    PASS
go vet ./...                                                    PASS
go test -count=1 -v ./internal/telemetry                       PASS 14 schemas / 24 valid / 36 invalid / golden hashes
npm test -- --run                                              PASS 21 files / 111 tests
npm run typecheck                                               PASS
npm run export:web                                              PASS
git diff --check                                                PASS
```

Go used the user-owned verified Go 1.27.1 executable at
`/home/raya/.local/opt/go1.27.1/bin/go`, without changing `go.mod` or
`go.sum`. The lead copied the final firmware source into an isolated temporary
project and ran ASan/UBSan sequentially with no diagnostics: 434/434 passed.

The previously uploaded SD image SHA-256 is
`30def8246ef8f34df336f4865f15788ce331b87713db67a0fcb7d45958e1a66a`.
The newly built combined SD+HTTPS image is not uploaded; its SHA-256 is
`68917fcc4b2a055551674247484d7860ab8c7b9ddda5de0669863cb0a000afbc`.
Selected-target disassembly reports a 32-byte `SlotProcessor::process` frame
and a `0xa40` (2,624-byte) `ProcessingPipeline::evaluate` frame; the former
1,664-byte and 5,376-byte frames no longer occur. These are compiler
estimates, not runtime high-water measurements.

Attached-only SD evidence on the ESP32-S3 N16R8 (MAC suffix `f5c150`) used
SD_MMC 1-bit, CLK=39, CMD=38, D0=40, and `format_if_mount_failed=false`:

```text
sd=PASS
bus=mmc1
n=256
hash=f987a1b5e0b87fc9e660f09967563a78de60df01dec65236945120eea61d4c56
us=106507021
t=31252201472
u=8290304
f=31243911168
a=PASS
OK
mode=RUN
config_version=1
config_hash=8872dc3107056e6ec4f0c1502e078454092fda2c2cf87e1db45d571841d9e540
```

The probe created and removed only its device-owned path
`/sigap/task9_sd_probe_f516a398.bin`. Archive append, reopen, recovery,
contains, reclaim, and exact file hash verification passed. This is attached
SD I/O evidence, not endurance, power-loss, or sensor evidence. LittleFS
reported a pre-existing corrupted directory pair and was not formatted or
erased. BLE pairing, WiFi association, HTTPS transmission, physical sensors,
heater, actuators, and power-loss tests remain NOT RUN or BLOCKED.

Status: **OPEN FOR INDEPENDENT REVIEW**. The branch contains software-test
coverage and integration evidence only. Wave 1 added the
commissioning runbooks in `docs/task9-commissioning.md` and
`docs/task9-sensor-commissioning.md`; no physical
commissioning or destructive hardware operation was performed.

## Mandatory delegation evidence

Wave 1 used exclusive worktrees and ownership:

- Firmware integration: commit `244a85a`, combined SD+HTTPS compile target.
- Storage/recovery: commit `6064ee2`, ARDUINO adapter short-write and interrupted
  reclaim regressions.
- Backend/security: commit `4423750`, operator ownership and bounded duplicate-key
  request validation plus SQLite/TLS integration coverage.
- Frontend/BLE: commit `aaf25ce`, evidence gating, BLE framing, disconnect
  recovery, bounded capabilities and target validation.

The lead cherry-picked these changes serially because platform builds share the
PlatformIO output directory and backend/frontend contracts have separate owners.
The independent audit at the pre-integration baseline passed Go, native, and
cppcheck; its first ASan/UBSan attempt was rejected because a shared PlatformIO
artifact race caused two missing-object errors. The lead reran the final source
in an isolated temporary project and obtained 434/434 with no diagnostics.

## Independent review remediation (`b48eac1` -> `75f9634`)

The independent source audit reproduced three software defects and added
regressions before the fixes were applied:

- Replay lookup ran before current remote authorization, allowing an exact
  replay to bypass a revoked principal. Authorization now runs before any
  replay lookup, while committed replay remains available for an authorized
  retry.
- A new COMMIT could replace an unreconciled prepared intent. The bounded
  memory store and the Arduino NVS store now reject a different prepared
  identity and fail closed on an invalid existing record. A matching prepared
  identity is idempotent.
- Raw USB calibration mutations could run while BLE or HTTPS owned the
  session. The application binds the shared controller to the maintenance
  router; raw USB mutations are rejected while another transport owns the
  session, while controller-internal typed dispatch uses an explicit internal
  boundary.

Focused regressions cover authorization revocation, prepared-intent
replacement, and USB/session contention. Final source is `75f96348f8b93a3ddead9b1f62f946523d701f29`.
The fixes preserve the stable
command identity rule: local monotonic deadlines are not replay identity.

## Latest remediation under review

Starting source: `c09c4663b3cf91082d711bbc7ef389cd019bd79b`.
The remediation at the final source head adds a durable COMMIT intent record before
profile activation. A lost result persistence is therefore retryable and can
be reconciled against the already active profile without running activation a
second time. The local monotonic deadline is no longer part of replay
identity; the authenticated backend request hash remains the stable identity.
The Arduino executor now requires `expected_profile_version` as a canonical
positive decimal string and emits the original profile identity for both first
and replayed COMMIT results. The compile-only
`sigap_n16r8_https_compile` environment checks the endpoint-enabled
composition with an invalid placeholder endpoint and empty CA, so it cannot
produce a usable runtime connection or expose credentials.

## Baseline and safety boundary

- Task 8 merge SHA: `6269b6c2532d25a93ac6b12454106613e17af911`.
- Task 9 branch: `feat/full-integration-hil-validation`.
- Reviewed Task 9 final source baseline before this remediation: `df5f5ec`.
- Final sensor-readiness verification commit: `0e7986bc625c7ba5fd6658bcf0570e90c026200b`.
- Hardware: ESP32-S3 N16R8 with onboard FAT32 microSD; no sensors, heater, or actuators.
- No firmware upload, reboot, SD mutation, NVS erase, unplug, power-cycle,
  power-cut, or physical commissioning was performed.
- Native fixtures use synthetic records and are not physical sensor evidence.

## Evidence matrix

| Area | Result | Fresh evidence and limitation |
| --- | --- | --- |
| Acquisition to processing/archive boundary | PASS (synthetic native) | `test_task9_integration`: attempt reaches processing/archive without stealing acquisition handoff. |
| Immutable batch and ACK lifecycle | PASS (synthetic native) | Exact bytes remain pending across retry; valid ACK reclaims; malformed ACK does not. |
| Archive recovery and fallback | PASS (existing native regression) | Full native and sanitizer suites include fault-injected archive paths; physical media recovery remains untested. |
| SD_MMC 1-bit | BUILD PASS | `sigap_n16r8_sd` compiles the confirmed GPIO39/38/40 policy; no physical SD I/O was run in this batch. |
| WiFi state machine | PASS (native tests) | Existing bounded reconnect/backoff tests remain green; no target association was attempted. |
| HTTPS boundary | PASS (native/backend tests) | Backend round-trip test uses an in-process authenticated service and exact bytes; no staging endpoint credentials were available. |
| BLE provisioning | PASS (web/native contract tests) | Browser protocol, secure-context, gesture, privacy, disconnect, and one-shot passkey handoff tests pass; physical advertising/pairing was not run. |
| USB passkey commissioning | PASS (native contract) | `maintenance enter` followed by `ble passkey` discloses the six-digit value once from the existing NVS-derived handoff; status/log/telemetry paths do not expose it. Hardware execution remains blocked. |
| Loopback HTTPS staging | PASS (software only) | Fresh Go 1.27.1 tests cover fail-closed TLS configuration and isolated SQLite; no staging process or device transmission was started. |
| Frontend dependency triage | PASS (bounded remediation) | Three narrow lockfile-backed overrides remove the production critical finding; remaining findings are documented toolchain/development risk and were not force-upgraded. |
| Backend ingest and current projection | PASS (software only) | Fresh Go `internal/e2e` and full tests cover durable ACK, replay dedupe, conflict, auth, export, late backlog, and filtered cursor; no staging process or device transmission was started. |
| Schemas and frozen hashes | PASS | Fresh Go semantic run compiled 14 schemas, checked 24 valid fixtures, rejected 36 invalid fixtures, and preserved both frozen hashes. |
| Firmware native suite | PASS | Fresh PlatformIO suite completed 434/434 test cases across 41 folders. |
| Firmware sanitizer suite | PASS | Isolated temporary project with `-fsanitize=address,undefined -fno-omit-frame-pointer`: 434/434, no diagnostics. |
| Cppcheck | PASS | `pio check -e native --fail-on-defect=low`, no defects. |
| Target builds | PASS | Standard targets and compile-only HTTPS targets pass; combined SD+HTTPS target uses RAM 52,260 and flash 1,358,761. |
| Frontend | PASS | 21 files/111 Vitest tests, TypeScript typecheck, and Expo web export pass. |
| Sensor software readiness | PASS (synthetic native) | All 13 channel adapters retain typed evidence and failure reasons; first-time profile paths are covered by native fakes. No physical coefficients are claimed. |
| Sensor physical calibration/safety | BLOCKED | No physical sensors, MQ-7 heater, or actuators are connected. |
| Physical BLE/WiFi commissioning | BLOCKED | Requires authorized target run, browser secure context, test network, and secure one-shot passkey handoff on the display-less board. Procedure: `docs/task9-commissioning.md`. |
| Physical SD I/O, endurance, 24 h/72 h soak | BLOCKED | Requires explicit hardware authorization and timestamped reconciliation. |
| Device-control result durability | PASS (software) | Current `SigapApp` binds the LittleFS `FsDeviceControlResultJournal`; 445 native tests, sanitizer regressions, and six target builds cover recovery and fail-closed startup. Physical reboot/result-post recovery remains separate. |

## Fresh command results

Toolchain: PlatformIO supplied by `uv tool run`; Node/npm dependencies
installed from the repository lockfile. Go ran from the isolated official
installation `/home/raya/.local/go1.27.1-verified/bin/go`.

```text
go1.27.1 test -count=1 ./...                  PASS
go1.27.1 test -race -count=1 ./...           PASS
go1.27.1 vet ./...                            PASS
go1.27.1 test ./internal/telemetry            PASS (14 schemas, 24 valid, 36 invalid, golden hashes)
npm test                                      PASS (21 files, 107 tests)
npm run typecheck                             PASS
npm run export:web                            PASS
pio test -e native                            PASS (430/430)
pio test -d . -c /tmp/platformio-sanitize.ini -e native
                                             PASS (430/430 cases, 41 folders, 0 failed)
pio check -e native --fail-on-defect=low     PASS (no defects)
pio run -e sigap_n16r8                       PASS (RAM 49,496; flash 1,105,033)
pio run -e sigap_n16r8_sd                    PASS (RAM 50,036; flash 1,178,117)
pio run -e sigap_n16r8_sd_spi                PASS (RAM 49,792; flash 1,157,613)
pio run -e sigap_bench                       PASS (RAM 49,496; flash 1,105,033)
pio run -e sigap_n16r8_https_compile         PASS (compile-only; RAM 51,824; flash 1,298,801)
git diff --check                             PASS
```

The official Go 1.27.1 toolchain was installed in a user-owned isolated
directory after checksum verification against official download metadata.
The commands above are fresh results from that executable; `go.mod` and
`go.sum` were not changed.

The frozen golden hashes remain:

```text
record_hash sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883
body_hash   sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c
```

The dependency audit before the bounded overrides reported 41 findings
(1 low, 11 moderate, 26 high, 3 critical), including one production critical.
The current lockfile-backed audit reports 38 findings overall
(11 moderate, 25 high, 2 critical) and 29 production-filtered findings
(10 moderate, 19 high, 0 critical). The remaining critical findings are
development-tooling exposure; major Expo/Metro upgrades were not forced into
this branch. The exact overrides and compatibility rationale are recorded in
`docs/task9-dependency-audit.md`.

## Task 9 additions

- `lib/ble/task9_integration.test.ts` covers Web Bluetooth capability and
  secure-context gating, user-gesture connection, bounded scan/retry/status,
  credential privacy, and disconnect recovery.
- `internal/e2e/task9_roundtrip_test.go` exercises the real migrated SQLite,
  telemetry archive service, authenticated HTTP router, exact replay, conflict,
  auth rejection, export, late backlog, and stable cursor behavior.
- `firmware/test/native/test_task9_integration/test_task9_integration.cpp`
  covers synthetic acquisition-to-archive flow, exact immutable retry, valid
  and malformed ACK behavior, bounded worker progress, and SD_MMC policy.
- `firmware/lib/calibration` accepts `UNCALIBRATED` only with sensor-specific
  physical evidence and retains bounded DS18B20 wire evidence.
- `firmware/lib/acquisition` permits explicit calibration-only DS18B20 ROM
  discovery and keeps missing-profile normal acquisition bus-free.
- `firmware/lib/maintenance` accepts bounded first-time engineering criteria
  and stages typed HX711, ultrasonic, MQ ADC, DS18B20, and SHT31 profiles only
  through validate/preview/commit.
- `firmware/lib/maintenance/src/maintenance/runtime_calibration.cpp` owns the
  calibration-profile builder and `runtime_diagnostics.cpp` owns the bounded
  sensor status formatter; the command router source remains below the
  repository file-size limit.
- `firmware/test/native/test_sensor_commissioning` runs parser-to-profile
  persistence regressions for ultrasonic, MQ ADC, DS18B20, and SHT31 address
  semantics; the existing maintenance test covers HX711.
- `docs/task9-sensor-commissioning.md` contains operator commands, physical
  reference requirements, diagnostics, and recovery steps.
- `firmware/lib/provisioning` and `firmware/lib/maintenance` implement the
  authorized `maintenance enter` then `ble passkey` one-shot handoff. The
  existing NVS-derived setup secret and BLE MITM settings are unchanged.
- `cmd/sigap/server_config.go` and `docs/task9-staging.md` define fail-closed,
  loopback-only TLS staging with an independent SQLite database.
- `tasks/plan-full-integration-hil-validation.md` and
  `docs/TASK9_Customization.md` define ownership, policy bounds, and physical
  evidence limits.

No production credentials, WiFi passwords, bearer tokens, or sensor readings
were added. No physical command, upload, reboot, pairing, WiFi change, NVS
mutation, or SD write was performed. Task 10 was not started.

## Review gate

This branch is ready for independent review of the software evidence. The
physical BLE/WiFi/HTTPS commissioning, sensor and heater evidence, physical
SD recovery, and endurance gates remain explicitly blocked rather than passed.

## Unified web calibration remediation

Fresh source head for this remediation is recorded by Git after the focused
BLE, backend, and web changes are integrated. The website now uses an
in-memory operator bearer token for authenticated device-control diagnostics
and commands. The token is never persisted in browser storage or sent to a
legacy research endpoint. The diagnostics endpoint returns the exact persisted
device result body; the web client accepts that raw body without reserializing
it and keeps the typed wrapper at the UI boundary. Capabilities are bounded
and sensor-specific, with units but no production thresholds.

The firmware BLE adapter translates `CAL` requests to the existing maintenance
grammar. Kriteria are applied as one authoritative `field value` command per
bounded `key=value` argument. Capture requires an explicit `tare`, `reference`,
`empty`, or `full` anchor. Validate, preview, and commit require the selected
sensor to remain the active session target. A BLE disconnect cancels only a
session started by that BLE adapter; it cannot cancel a USB-owned session.
Unsupported capability/history/rollback operations are rejected rather than
fabricating a response until the typed workflow API exposes those operations.

Fresh software evidence:

```text
pio test -e native                                      PASS 409/409
focused BLE/calibration tests                          PASS 18/18
pio check -e native --fail-on-defect=low                PASS
sigap_n16r8                                              PASS RAM 49,496; flash 1,087,001
sigap_n16r8_sd                                           PASS RAM 50,036; flash 1,160,389
sigap_n16r8_sd_spi                                       PASS RAM 49,792; flash 1,140,017
sigap_bench                                              PASS RAM 49,496; flash 1,087,001
go test -count=1 ./...                                   PASS
go test -race -count=1 ./...                             PASS
go vet ./...                                             PASS
npm test -- --run                                       PASS 21 files / 103 tests
npm run typecheck                                        PASS
npm run export:web                                       PASS
telemetry schemas/fixtures/golden hashes                 PASS 14/24/36 and unchanged
git diff --check                                         PASS
```

This historical section predates the isolated sanitizer run recorded above;
the current integrated source has fresh 434/434 sanitizer evidence. Physical
BLE pairing, WiFi change/reconnect, HTTPS device polling,
operator-provider deployment, sensor evidence, heater operation, NVS, SD, and
power tests remain blocked by the explicit hardware boundary.

The following paragraphs record the pre-composition state and are retained as
history. They are superseded by the fresh remediation section below. The
remaining software integration gate at that earlier state was firmware-side
HTTPS polling of the
Go command queue. The queue, authenticated operator/device endpoints, exact
diagnostic body, idempotent command persistence, expiry, and native tests are
implemented. No firmware network worker currently claims remote calibration
commands, so WEB-CAL remote delivery is not marked complete until that adapter
is wired and tested with the existing nonblocking network boundary.

The latest closure attempt at that earlier state added a CAL1 typed response
contract, typed BLE
preview/profile identity handling, web command status polling with refresh
recovery, database-backed operator sessions and per-device authorization,
lease recovery, structured committed-result validation, and a bounded native
`DeviceControlPoller` seam. The poller seam has focused native coverage, but
the Arduino HTTP implementation and shared calibration executor are still not
bound into `SigapApp`. Operator authorization remains fail-closed and requires
deployment provisioning. Task 9 therefore remains HOLD.

## Fresh unified web calibration remediation (`bf37efcf0ea86ea9848d57d395f729f58b77b6ef`)

The current source adds the production-side composition points that were
missing from the previous ledger. `SigapApp` owns one calibration controller,
the BLE adapter receives that controller, and the Arduino device-control HTTP
transport/executor/poller are composed when endpoint and CA build policy is
provided. The default target intentionally omits those macros, so it remains
fail-closed rather than silently using an unverified endpoint. All mutation
paths retain bounded buffers and share controller ownership; no separate
calibration manager was added.

Fresh verification from the current working tree:

```text
pio test -e native                                      PASS 424/424
temporary ASan/UBSan PlatformIO config                 PASS 424/424
pio check -e native --fail-on-defect=low                PASS, no defects
pio run -e sigap_n16r8                                  PASS RAM 49,496; flash 1,103,173
pio run -e sigap_n16r8_sd                               PASS RAM 50,036; flash 1,176,225
pio run -e sigap_n16r8_sd_spi                           PASS RAM 49,792; flash 1,155,685
pio run -e sigap_bench                                  PASS RAM 49,496; flash 1,103,173
go test -count=1 ./...                                  PASS
go test -race -count=1 ./...                            PASS
go vet ./...                                             PASS
npm test                                                PASS 21 files / 107 tests
npm run typecheck                                        PASS
npm run export:web                                       PASS
telemetry package: 14 schemas / 24 valid / 36 invalid  PASS
frozen record/body hashes                               PASS, unchanged
git diff --check                                         PASS
```

The telemetry package explicitly ran `TestAllTelemetrySchemasCompile`, both
fixture corpora, and the canonical record/body hash tests. The native suite
includes controller identity/principal replay, durable committed result replay
across controller recreation, proposed-profile expected identity, BLE
shared-owner routing, strict device-control command/result framing, expiry,
lost-result recovery, and the existing sensor/profile/archive regressions.

Remaining acceptance gaps are deployment and hardware boundaries, not relabeled
test results: the default target has no endpoint/CA macros, no non-production
HTTPS staging transmission was run, no browser Web Bluetooth pairing was run,
and no physical sensor/NVS/SD operation was performed. Operator auth is
fail-closed in the backend but the current browser journey still supplies
short-lived bearer/CSRF values in memory rather than using a deployed operator
identity provider. The NVS replay intent is persisted before profile activation
and the terminal profile identity afterward; a fault between those boundaries
is reconciled only when active identity matches the intent. Physical power-loss
execution remains untested. This remediation remains pending final commit/push
and independent review. Task 10 was not started.

## Current cross-layer remediation at `8995590`

This section supersedes the earlier diagnostic and operator-status notes.
The operator command read route now returns a dedicated durable status
contract with command identity, sensor, session, status, result bytes, error
code, expiry, attempt, and lifecycle timestamps. The firmware delivery
envelope remains separate and does not expose operator readback fields.

Diagnostics now read only the telemetry-v3 sample projection. START, PREVIEW,
and COMMIT results cannot be returned as sensor evidence. The response keeps
sensor, stream, quality, reason, raw and converted evidence, freshness, source
sequence, and profile identity; an absent projection is unavailable.

BLE capability requests require the selected sensor. START forwards bounded
sensor setup parameters through the typed maintenance boundary used by HTTPS,
and COMMIT requires the preview's exact profile version and SHA-256 identity.
The BLE response includes the committed profile version and hash. The web
wizard advances from device-confirmed operation outcomes through DIAGNOSTICS,
START, SET_CRITERIA, CAPTURE, VALIDATE, PREVIEW, COMMIT, and DONE without
treating queue delivery as calibration completion.

Fresh focused evidence for this revision:

```text
Go device-control/http tests                         PASS
Go internal/http, internal/e2e, cmd/sigap tests       PASS
Frontend BLE protocol and wizard tests                PASS 19 tests
Frontend typecheck                                     PASS
PlatformIO native                                      PASS 430/430
HTTPS-enabled Arduino composition build               PASS
git diff --check                                       PASS
```

Native and HTTPS-enabled builds remain software evidence only. No physical
BLE pairing, WiFi change, endpoint transmission, sensor calibration, NVS/SD
mutation, or power-loss test was performed. A staging endpoint and operator
identity provider still require external provisioning and remain BLOCKED.

## Fresh final verification at `8995590`

The final integrated working tree was verified serially after the BLE setup,
SHT31 address validation, and structured rejection fixes:

```text
pio test -e native --without-uploading                         PASS 430/430
temporary ASan/UBSan PlatformIO config                         PASS 430/430
pio check -e native --fail-on-defect=low                        PASS, no defects
pio run -e sigap_n16r8                                          PASS RAM 49,496; flash 1,107,045
pio run -e sigap_n16r8_sd                                       PASS RAM 50,036; flash 1,180,105
pio run -e sigap_n16r8_sd_spi                                   PASS RAM 49,792; flash 1,159,605
pio run -e sigap_bench                                          PASS RAM 49,496; flash 1,107,045
pio run -e sigap_n16r8_https_compile                            PASS RAM 51,824; flash 1,300,933
go test -count=1 ./...                                          PASS
go test -race -count=1 ./...                                    PASS
go vet ./...                                                    PASS
npm test -- --run                                              PASS 21 files / 108 tests
npm run typecheck                                               PASS
npm run export:web                                              PASS
go test -count=1 -v ./internal/telemetry                       PASS 14 schemas / 24 valid / 36 invalid / golden hashes
git diff --check                                                PASS
```

The native BLE adapter regression covers sensor-specific START setup,
SHT31 address binding, mandatory preview identity for COMMIT, structured
rejection, and shared-controller ownership. Go HTTP tests cover durable
operator status readback and the telemetry-only diagnostic source. These are
software and synthetic evidence; live BLE, private HTTPS transmission,
operator-provider provisioning, physical sensors, NVS/SD writes, and power
loss remain BLOCKED or NOT RUN under the hardware boundary.

The final source delta at `a2f9d35` routes diagnostics through the existing
chronology-aware telemetry current projection instead of insertion order. This
preserves same-boot ordering and explicit cross-boot unknown ordering for the
diagnostic response. Focused device-control, archive-query, e2e, and HTTP
tests, followed by full Go, race, and vet runs, passed after this change.

## Fresh integrated journey evidence at `83f22a3`

The current working changes add production-composition integration regressions
without changing firmware source. Journey B is exercised through an ephemeral
TLS server, the migrated SQLite schema, the real operator/device authorizers,
command lease, six lifecycle commands, durable operator readback, exact
COMMIT replay, changed-result conflict, and CSRF rejection:

```text
go test -count=1 ./internal/e2e -run TestTask9RemoteCalibrationLoopbackJourney -v  PASS
  6 lifecycle subtests; ephemeral TLS; isolated SQLite; no external network
npm test -- --run lib/ble/task9_integration.test.ts                         PASS
  6 tests, including synthetic CAL1 START -> SET_CRITERIA -> CAPTURE ->
  VALIDATE -> PREVIEW -> COMMIT with exact preview identity and wizard DONE
```

The Journey B device is an explicit synthetic wire-contract seam, and the
Journey A GATT server is an in-process fake. They prove software contracts,
not ESP32 HTTPS transmission or physical Web Bluetooth pairing. The full
fresh suite listed above remains applicable because these changes are tests and
documentation only; a final serial rerun is required after commit.
