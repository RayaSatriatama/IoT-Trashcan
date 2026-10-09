# Task 9 verification ledger

Status: **OPEN FOR INDEPENDENT REVIEW**. The branch contains software-test
coverage and integration evidence only. The current preflight added the
commissioning runbooks in `docs/task9-commissioning.md` and
`docs/task9-sensor-commissioning.md`; no physical
commissioning or destructive hardware operation was performed.

## Baseline and safety boundary

- Task 8 merge SHA: `6269b6c2532d25a93ac6b12454106613e17af911`.
- Task 9 branch: `feat/full-integration-hil-validation`.
- Reviewed Task 9 source HEAD before this verification: `0fe068a`.
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
| Loopback HTTPS staging | PASS (Go config/tests) | TLS-required configuration fails closed for incomplete/missing certificate pairs and supports an isolated SQLite path; no staging process or device transmission was started. |
| Frontend dependency triage | PASS (bounded remediation) | Three narrow lockfile-backed overrides remove the production critical finding; remaining findings are documented toolchain/development risk and were not force-upgraded. |
| Backend ingest and current projection | PASS (synthetic integration) | `internal/e2e` exercises durable ACK, replay dedupe, conflict, auth, export, late backlog, and filtered cursor. |
| Schemas and frozen hashes | PASS | 14 schemas compile; 24 valid and 36 invalid fixtures; frozen record/body hashes unchanged. |
| Firmware native suite | PASS | 385/385 test cases on the sensor-readiness source. |
| Firmware sanitizer suite | PENDING FRESH RUN | Previous 374/374 evidence is historical and must be rerun after sensor-readiness changes. |
| Cppcheck | PASS | `pio check -e native --fail-on-defect=low`, no defects. |
| Target builds | PASS | `sigap_n16r8`, `sigap_n16r8_sd`, `sigap_n16r8_sd_spi`, and `sigap_bench`. |
| Frontend | PASS | 19 files/92 Vitest tests, TypeScript typecheck, and Expo web export pass. |
| Sensor software readiness | PASS (synthetic native) | All 13 channel adapters retain typed evidence and failure reasons; first-time profile paths are covered by native fakes. No physical coefficients are claimed. |
| Sensor physical calibration/safety | BLOCKED | No physical sensors, MQ-7 heater, or actuators are connected. |
| Physical BLE/WiFi commissioning | BLOCKED | Requires authorized target run, browser secure context, test network, and secure one-shot passkey handoff on the display-less board. Procedure: `docs/task9-commissioning.md`. |
| Physical SD I/O, endurance, 24 h/72 h soak | BLOCKED | Requires explicit hardware authorization and timestamped reconciliation. |

## Fresh command results

Toolchain: Go `go1.27.1 linux/amd64`; PlatformIO supplied by `uv tool run`;
Node/npm dependencies installed from the repository lockfile.

```text
go test -count=1 ./...                         PASS
go test -race -count=1 ./...                  PASS
go vet ./...                                  PASS
go test ./internal/telemetry (schema/golden)  PASS (14/24/36)
npm test                                      PASS (19 files, 92 tests)
npm run typecheck                             PASS
npm run export:web                            PASS
pio test -e native                            PASS (385/385)
ASan/UBSan native folders                    PENDING FRESH RUN (previous 374/374 is historical)
pio check -e native --fail-on-defect=low     PASS (no defects)
pio run -e sigap_n16r8                       PASS (RAM 49,496; flash 1,074,473)
pio run -e sigap_n16r8_sd                    PASS (RAM 50,036; flash 1,148,245)
pio run -e sigap_n16r8_sd_spi                PASS (RAM 49,792; flash 1,127,693)
pio run -e sigap_bench                       PASS (RAM 49,496; flash 1,074,473)
git diff --check                             PASS
```

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
