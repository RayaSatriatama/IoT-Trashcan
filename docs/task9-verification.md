# Task 9 verification ledger

Status: **OPEN FOR INDEPENDENT REVIEW**. The branch contains software-test
coverage and integration evidence only. The current preflight added the
commissioning runbooks in `docs/task9-commissioning.md` and
`docs/task9-sensor-commissioning.md`; no physical
commissioning or destructive hardware operation was performed.

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
| Firmware native suite | PASS | Fresh PlatformIO suite completed 430/430 test cases across 41 folders. |
| Firmware sanitizer suite | PASS | Fresh ASan/UBSan run completed 430/430 cases with no diagnostics. |
| Cppcheck | PASS | `pio check -e native --fail-on-defect=low`, no defects. |
| Target builds | PASS | `sigap_n16r8`, `sigap_n16r8_sd`, `sigap_n16r8_sd_spi`, `sigap_bench`, and compile-only `sigap_n16r8_https_compile`. |
| Frontend | PASS | 21 files/107 Vitest tests, TypeScript typecheck, and Expo web export pass. |
| Sensor software readiness | PASS (synthetic native) | All 13 channel adapters retain typed evidence and failure reasons; first-time profile paths are covered by native fakes. No physical coefficients are claimed. |
| Sensor physical calibration/safety | BLOCKED | No physical sensors, MQ-7 heater, or actuators are connected. |
| Physical BLE/WiFi commissioning | BLOCKED | Requires authorized target run, browser secure context, test network, and secure one-shot passkey handoff on the display-less board. Procedure: `docs/task9-commissioning.md`. |
| Physical SD I/O, endurance, 24 h/72 h soak | BLOCKED | Requires explicit hardware authorization and timestamped reconciliation. |

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

The repository does not define a sanitizer PlatformIO environment in this
checkout, so a new ASan/UBSan run was not executable without introducing an
unreviewed build configuration. Historical sanitizer evidence is not relabeled
as fresh. Physical BLE pairing, WiFi change/reconnect, HTTPS device polling,
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
