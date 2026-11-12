# Task 9: unified web calibration and hardware telemetry integration

## Authority and boundary

- PRD/TSD firmware SIGAP v1.3/v1.2 and telemetry-v3 contracts remain normative.
- Existing `CalibrationSession`, `CalibrationEvidenceCache`, `ProfileManager`,
  `CommandRouter`, BLE F1/R1 framing, and telemetry archive remain authoritative.
- This slice adds transport adapters and operator UI. It does not add physical
  sensor evidence, production coefficients, HTTP firmware upload, or Task 10.

## Shared contract before parallel work

1. A bounded, versioned `CalibrationCommandEnvelope` identifies device, sensor,
   session, request, operation, expiry, expected active version/hash, and typed
   criteria/reference values.
2. A bounded `CalibrationResultEnvelope` carries status, error, state, accepted
   and rejected counts, evidence freshness, proposal/profile identity, and
   provenance. Raw readings are read-only evidence and never operator input.
3. One instance-owned calibration controller arbitrates all transports. A start
   reserves one session owner; read-only status/evidence is allowed to other
   transports; mutating operations from another owner return `SESSION_BUSY`.
4. USB, BLE, and HTTPS are adapters that call the same controller operations.
   Existing USB syntax remains a recovery interface and is not reimplemented.

## Work packages

- A: firmware shared controller and bounded command/result codec, with native
  tests for parity, expiry, arbitration, idempotency, and profile guards.
- B: BLE calibration adapter and web Bluetooth client, with framing, session,
  authorization, disconnect, and unsupported-browser tests.
- C: Go persistent device-command queue and authenticated operator/device APIs,
  with SQLite restart, retry, expiry, ownership, and idempotency tests.
- D: website diagnostics, calibration wizard, and telemetry-v3 current/archive
  views, with component and client tests. No credentials in browser storage.
- E: parent integration, normative docs, acceptance matrix, resource review,
  and full native/Go/web verification.

## Acceptance mapping

WEB-CAL-01..18 are documented in the PRD/TSD update and mapped to source,
focused tests, integrated tests, and explicit physical-test limitations in
`docs/task9-verification.md`. A requirement is not marked PASS from a mock
alone when it requires BLE/WiFi/sensor hardware.

## Verification sequence

1. Focused tests per work package.
2. Firmware native, sanitizer, cppcheck, and target builds.
3. Go unit/race/vet, schema/golden checks.
4. Frontend test, typecheck, and web export.
5. Review working tree, diff check, resource evidence, and PR state.

Physical operations, merge, and Task 10 remain outside this task.
