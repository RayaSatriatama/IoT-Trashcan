# Task 9 web calibration runbook

This document describes the software workflow. It does not authorize physical
commissioning. Sensor, BLE, WiFi, HTTPS target, NVS, and SD operations remain
separate acceptance gates.

## Transport model

The website sends typed commands through either a local Web Bluetooth adapter or
the authenticated remote device-control queue. Both adapters target the same
firmware calibration session and evidence cache. USB maintenance remains the
secondary local recovery path. A transport cannot submit raw sensor readings,
coefficients, NVS commands, reset commands, GPIO changes, or heater commands.

## Workflow

1. Select a device and one of the 13 channels.
2. Read diagnostics and confirm raw evidence, quality/reason, freshness,
   identity/CRC/completion flags, and active profile identity.
3. Start a session. A different mutating transport receives `SESSION_BUSY`.
4. Enter explicit engineering criteria and physical reference values. Values
   are typed and unit-checked; missing references keep the session configuring.
5. Capture genuine device observations. Synthetic native fixtures are labelled
   `TEST_ONLY` and cannot become production profiles.
6. Validate stability and identity, then request preview.
7. Review the proposed typed profile and evidence/provenance identity.
8. Commit only while the proposal, evidence sequence, active version/hash, and
   expiry still match. Failed commit preserves the previous active profile.
9. Cancel or allow timeout to discard session-owned overlay and draft state.
10. Reload/status/export/rollback through the existing profile lifecycle.

## Operator security

The operator API requires an authenticated operator session with per-device
authorization and CSRF/origin/rate-limit controls. Device bearer tokens are
never sent to the browser. Web Bluetooth requires a secure context, explicit
user gesture, authenticated MITM GATT, and device-local proof of possession.
Remote commands are durable before device acknowledgement, expire, and are
idempotent by request ID and request hash.

## Sensor-specific references

- HX711: tare and an explicitly known positive reference mass.
- Ultrasonic: validated echo conversion and empty/full distances.
- MQ: validated ADC/divider parameters and phase policy; no ppm/R0/OGI.
- MQ-7: validated heater profile before any heater-dependent operation.
- DS18B20: discovered ROM/CRC, completion, resolution, and explicit reference
  temperature.
- SHT31: fixed address `0x44` or `0x45`, independent frame CRCs, and explicit
  temperature/humidity references.

No value is fabricated when a sensor is disconnected, stale, uncalibrated, or
missing a profile. Operational conversion remains unavailable in those states.

## WEB-CAL acceptance evidence

| ID | Evidence target | Physical limitation |
| --- | --- | --- |
| WEB-CAL-01..09 | Native controller, profile builder, and frontend workflow tests | Physical references not connected |
| WEB-CAL-10..12 | Auth/session/idempotency tests and BLE contract tests | BLE pairing deferred |
| WEB-CAL-13..14 | Profile provenance and telemetry-v3 API tests | No live device/backend |
| WEB-CAL-15..18 | Offline, USB recovery, synthetic deterministic tests | Physical HIL deferred |

## Remediation evidence matrix

| Requirement | Implementation | Fresh evidence | Result |
| --- | --- | --- | --- |
| Shared bounded lifecycle and owner arbitration | `calibration_transport.*` contract plus existing `CommandRouter` session/evidence/profile lifecycle | `test_calibration_transport`, maintenance regressions | PASS in native contract tests; controller still requires final composition wiring |
| Local Web Bluetooth calibration | BLE F1/R1 parser, authenticated session, `BleProvisioningSession`, `CommandRouterBleCalibrationBackend`, TypeScript client | BLE protocol tests, adapter test, 415 native suite, 103 web tests | PASS software-only |
| Authoritative criteria/capture grammar | Adapter translates key/value criteria and explicit anchors to maintenance commands | `test_calibration_ble_adapter`, protocol tests | PASS for supported maintenance operations |
| Sensor diagnostics | Operator-authenticated device-control diagnostics returns exact stored device result body | device-control HTTP/store tests | PASS API contract; no live device evidence |
| Typed capabilities | Bounded sensor-specific endpoint exposes units and required fields without thresholds | capabilities HTTP regression | PASS structural contract |
| Remote command durability | SQLite queue, idempotency hash, lease, expiry, session ownership, device result transitions | device-control store/HTTP tests and Go race suite | PASS backend queue; firmware poller seam tested, production transport/executor wiring remains open |
| Website wizard | `CalibrationWizard`, diagnostics panel, bounded command polling, typed sensor capabilities, operator token in memory, BLE and HTTPS paths | 21 Vitest files / 103 tests, typecheck, web export | PASS UI software; live operator provider unavailable |
| Exact evidence and no synthetic production readings | raw device result bytes preserved; native fixtures labelled synthetic | telemetry and device-control tests | PASS software boundary |
| Physical commissioning | BLE pairing, WiFi, HTTPS device polling, sensor references, heater | no physical operations authorized | BLOCKED |

The authoritative USB maintenance parser remains the recovery path. BLE
capabilities/history/rollback requests are rejected until a typed workflow
implementation exists for those operations; no fabricated response is used.
Remote HTTPS commands are persisted and authenticated on the backend. A
bounded firmware poller seam and replay tests exist, but the Arduino HTTP
implementation and shared calibration executor are not yet bound into
`SigapApp`. Production operator authorization also requires deployment
configuration. This document therefore does not claim Task 9 complete.
