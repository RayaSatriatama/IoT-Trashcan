# Implementation Plan: SIGAP Firmware Configuration Foundation

## Baseline and scope

- Baseline: `main` at `8cba1442ea974fe2e7943cb241259c823630ae01`.
- Scope: PlatformIO skeleton, compile-time board/build constants, typed `AppConfig`, pure validation, deterministic codec, native-testable A/B persistence, ConfigManager lifecycle, SecretStore boundary, and minimal composition root.
- Explicitly deferred: sensors, calibration, FIS, safety, logger/archive, telemetry transport, HTTP, Wi-Fi provisioning, BLE, OTA, backend changes, migration 005, and dashboard changes.

## Architecture decisions

- Keep firmware under `firmware/` as an independent PlatformIO compilation boundary.
- Use C++17-compatible code supported by the pinned Arduino/native toolchains; avoid C++20/23-only facilities.
- Keep `BoardConfig`, `BuildConfig`, firmware version, and factory defaults compile-time typed constants.
- Model `AppConfig` as value types with only schema/version, sampling, telemetry, and storage groups. Secrets, calibration/model profiles, GPIO, runtime state, and wire semantics remain outside it.
- Use a fixed-size little-endian codec with an explicit envelope, bounded payload, and SHA-256 identity. No raw struct persistence and no unbounded hot-path allocation.
- Abstract slot storage behind an in-memory test double and a target NVS adapter. The pointer changes only after complete inactive-slot write, readback, decode, validation, and hash verification.
- Keep `ConfigManager` instance-owned. The active snapshot is read-only to consumers; draft commit sets `REBOOT_REQUIRED` without mutating the current active object.
- Expose only a non-plaintext SecretStore boundary; no credentials or token fixtures.

## Task list

### Phase 1: Platform foundation and public types

- [ ] Add PlatformIO environments, partition CSV, board pins, build/version headers, defaults, minimal `main.cpp`, and `SigapApp` composition root.
  - Acceptance: target/native environments and required tree exist; no sensor dependency; `main.cpp` owns one explicit application-lifetime object.
  - Verification: PlatformIO config inspection; native compile/test command.
- [ ] Add typed config types, validation errors, defaults, and pure validator.
  - Acceptance: schema/version, timing, retry ordering, batch hard bound, quota reserve, and documented cross-field constraints are enforced without side effects.
  - Verification: native tests for valid defaults and every required invalid case.

### Phase 2: Deterministic identity and persistence

- [ ] Add deterministic codec and SHA-256 boundary with malformed/truncated/schema rejection.
  - Acceptance: stable bytes/hash for equal logical values, explicit schema/version, bounded output, round-trip, no secrets.
  - Verification: native byte/hash, round-trip, malformed, truncation, and secret-exclusion tests.
- [ ] Add native-testable A/B slot store, target NVS adapter boundary, and ConfigManager lifecycle.
  - Acceptance: fallback, provisioned `CONFIG_REQUIRED`, inactive-slot write/readback verification, last-step pointer switch, failed commit safety, version increment, draft immutability, and reboot activation.
  - Verification: in-memory fault-injection tests covering write/readback/hash/corruption/pointer failures and public lifecycle state.

### Checkpoint: foundation

- [ ] Focused native tests pass with zero failures.
- [ ] Native compile succeeds with warnings reviewed.
- [ ] Target build is attempted if PlatformIO is available; unavailable hardware is explicitly deferred.
- [ ] Memory/resource sizes are measured from the integrated code.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| PlatformIO is unavailable in the host | Keep target config exact and run deterministic native compiler checks; report target build as unverified. |
| Arduino NVS APIs are unavailable in native tests | Isolate them behind a small slot-store interface and use an in-memory implementation for all behavior tests. |
| SHA-256 implementation differs across target/native | Use one bounded portable implementation and golden digest tests. |
| Config payload exceeds the NVS slot budget | Compile-time payload bound, explicit encode failure, and static size reporting. |
| Merge of an interrupted commit is mis-modeled | Make pointer write the final operation and inject each failure point in the native store. |

## Traceability targets

- PRD §§2A, 7, 7A-7D, 8, 12 T1; AC01, AC11, AC30-AC33, and applicable AC34/AC35 interfaces.
- TSD §§2, 3, 3A, 14, 15.1-15.7, 15.12-15.13, and 16.
