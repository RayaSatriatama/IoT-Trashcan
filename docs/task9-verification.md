# Task 9 verification ledger

Status: **OPEN FOR INDEPENDENT REVIEW**. The branch contains software-test
coverage and integration evidence only. No physical commissioning or
destructive hardware operation was performed.

## Baseline and safety boundary

- Task 8 merge SHA: `6269b6c2532d25a93ac6b12454106613e17af911`.
- Task 9 branch: `feat/full-integration-hil-validation`.
- Final Task 9 HEAD: `701431011402669f88a8d8a4e7a4e7c89cb5c428`.
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
| BLE provisioning | PASS (web/native contract tests) | Browser protocol, secure-context, gesture, privacy, and disconnect tests pass; physical advertising/pairing was not run. |
| Backend ingest and current projection | PASS (synthetic integration) | `internal/e2e` exercises durable ACK, replay dedupe, conflict, auth, export, late backlog, and filtered cursor. |
| Schemas and frozen hashes | PASS | 14 schemas compile; 24 valid and 36 invalid fixtures; frozen record/body hashes unchanged. |
| Firmware native suite | PASS | 370/370 test cases on final Task 9 source. |
| Firmware sanitizer suite | PASS | ASan/UBSan configuration ran all 37 native test folders, 0 failures. |
| Cppcheck | PASS | `pio check -e native --fail-on-defect=low`, no defects. |
| Target builds | PASS | `sigap_n16r8`, `sigap_n16r8_sd`, `sigap_n16r8_sd_spi`, and `sigap_bench`. |
| Frontend | PASS | 19 files/92 Vitest tests, TypeScript typecheck, and Expo web export pass. |
| Sensors/calibration/safety | BLOCKED | No physical sensors, MQ-7 heater, or actuators are connected. |
| Physical BLE/WiFi commissioning | BLOCKED | Requires authorized target run, browser secure context, and test network. |
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
pio test -e native                            PASS (370/370)
ASan/UBSan native folders                    PASS (37 folders, 0 failed)
pio check -e native --fail-on-defect=low     PASS (no defects)
pio run -e sigap_n16r8                       PASS (RAM 49,496; flash 1,073,557)
pio run -e sigap_n16r8_sd                    PASS (RAM 50,036; flash 1,147,337)
pio run -e sigap_n16r8_sd_spi                PASS (RAM 49,792; flash 1,126,773)
pio run -e sigap_bench                       PASS (RAM 49,496; flash 1,073,557)
git diff --check                             PASS
```

The frozen golden hashes remain:

```text
record_hash sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883
body_hash   sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c
```

The frontend install reports 41 npm audit findings in transitive packages
(1 low, 11 moderate, 26 high, 3 critical). No `npm audit fix` was run and no
lockfile or production dependency was changed.

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
- `tasks/plan-full-integration-hil-validation.md` and
  `docs/TASK9_Customization.md` define ownership, policy bounds, and physical
  evidence limits.

No production credentials, WiFi passwords, bearer tokens, or sensor readings
were added. Task 10 and all excluded durable/network migration work were not
started.

## Review gate

This branch is ready for independent review of the software evidence. The
physical BLE/WiFi/HTTPS commissioning, sensor and heater evidence, physical
SD recovery, and endurance gates remain explicitly blocked rather than passed.
