# Task 9 Final Sensor Remediation

## Global constraints

- Preserve the current branch and unrelated changes.
- No physical operations: no upload, reboot, NVS mutation, BLE/WiFi, SD writes, sensor wiring, heater, or actuator activity.
- PRD/TSD and frozen telemetry contracts are authoritative.
- Diagnostics must reuse bounded acquisition/calibration evidence; no second consumer of acquisition handoff.
- Preview must be side-effect free and commit must reject stale proposals.
- Synthetic readings are test-only and explicitly labelled.

## Task 1: bounded per-sensor diagnostics

Implement read-only USB diagnostics for the selected sensor through the existing maintenance parser/runtime and evidence cache. Include quality/reason, raw presence, freshness, protocol flags, profile identity, calibration state and counts, with response bounds and secret redaction. Add focused parser/runtime tests and document exact commands.

## Task 2: pure calibration proposal and preview

Extract one validated, side-effect-free proposal builder used by preview and commit. Report typed family-specific parameters, preserve A/B persistence and provenance, reject stale evidence/reference/active-profile changes, and add focused tests for no-write preview, accuracy, interrupted commit and repeat calibration.

## Task 3: fresh Go and contract verification

Discover or install official Go 1.27.1 in a user-owned isolated directory without changing system installation, go.mod or go.sum. Run Go tests, race tests, vet, schema/fixture and frozen hash checks; record blockers honestly if unavailable.

## Task 4: integrated commissioning regression and documentation

Exercise the production USB parser/runtime composition from empty profile store through synthetic evidence, criteria, capture, validate, preview, commit, reload and converted output for supported families. Add negative/recovery cases and update guide, verification report, plan/todo and readiness matrix.
