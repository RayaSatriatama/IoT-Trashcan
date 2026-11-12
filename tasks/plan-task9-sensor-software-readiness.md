# Task 9 sensor software readiness plan

## Task 1: calibration evidence and first-time profile lifecycle

Own calibration session/cache and maintenance-facing evidence contracts.
Allow raw, physically valid `UNCALIBRATED` attempts into calibration without
accepting disconnected, stale, CRC-invalid, identity-invalid, or heater-invalid
evidence. Build typed first profiles for HX711, ultrasonic, MQ ADC, DS18B20,
and SHT31 only after explicit criteria and references. Add native tests.

## Task 2: DS18B20 discovery and ultrasonic bootstrap

Own target sensor adapters and native protocol fakes. Permit DS18B20 ROM
discovery only during an explicit calibration session when no profile exists;
retain strict CRC/conversion checks and no operational conversion. Permit
ultrasonic evidence conversion only after explicit engineering conversion
parameters are configured. Add protocol and acquisition tests.

## Task 3: maintenance workflow and persistence integration

Own parser/router commands for engineering parameters, diagnostics, capture,
validate, preview, commit, rollback, and profile readback. Preserve A/B atomic
ProfileManager behavior and unrelated channel operation. Add command and
empty-store end-to-end tests.

## Task 4: integration, documentation, and verification

Lead integrates the prior slices, adds the 13-channel readiness matrix and
operator commissioning guide, runs native/sanitizer/cppcheck/target/Go/web
checks, and updates the Task 9 verification ledger.
