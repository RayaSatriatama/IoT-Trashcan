# Task 5 remediation verification

## Scope

This remediation is based on Task 4 merge `2d886d6dc0843bd932a1da73eb233713c559d032`
and starts from Task 5 checkpoint `89e1e6e36ce0b01a3e9e127970117dec0d37bcc4`.
It does not start Task 6. No sensors, outputs, or physical HIL fixtures are
available, so evidence is native and target-build based.

## Requirement traceability

| Requirement | Implementation | Regression evidence |
| --- | --- | --- |
| Processing cycles and bounded handoff | `firmware/src/app/processing_runtime.h`, `sigap_app.cpp` | runtime observer/cycle/handoff tests |
| Model customization and activation | `firmware/lib/models/src/models/model_lifecycle.*`, `maintenance/model_runtime.*` | model lifecycle and parser/router transfer tests |
| HX711 latest-attempt readiness | `processing_engine.h` | READY to CALIBRATING/timeout regression |
| Persistent B3 safety latch | `processing_engine.h`, `safety.h` | pipeline/runtime safety tests and two-clear-cycle tests |
| Snapshot/provenance isolation | processing snapshot and result factory | profile conflict, source context, and model snapshot tests |
| MQ model evidence | typed ADC profile capture and mV metrics | valid ADC evidence test; unavailable values remain explicit |

## Policy boundaries

AppConfig schema v1 and factory defaults are unchanged. Sampling and telemetry
cadence remain AppConfig policy. Calibration and scientific values remain typed
versioned profiles. Board pins and protocol constants remain build-time. Secrets
remain in SecretStore. Runtime state, safety latch, windows, and handoff rings
are instance-owned RAM state. TEST_ONLY model fixtures are never installed by
target composition.

## Verification record

Fresh verification on the integrated source completed as follows:

- Native PlatformIO tests: **228/228 passed**.
- Native ASan/UBSan PlatformIO copy: **227/227 passed**; the additional final
  test is a compile-time footprint assertion and production source was
  unchanged by that test-only addition.
- Cppcheck: **No defects found** with `--fail-on-defect=low`.
- Target builds: `sigap_n16r8`, `sigap_n16r8_sd`, and `sigap_bench` all passed.
- Go 1.27.1 isolated toolchain: telemetry tests, full `go test ./...`, and
  `go vet ./...` passed.
- Schema conformance: all 14 Draft 2020-12 schemas compiled; 24 valid fixtures
  passed schema validation and 36 invalid fixtures retained expected Go
  rejection codes. Frozen record and exact-body hash tests passed.
- Physical HIL: not required and not performed; no sensor/output wiring exists.

## Resource notes

Final target footprint is 72,584 B RAM / 398,417 B flash for each of the three
profiles. Native measured sizes are: `ProcessingRuntime` 13,392 B,
`ProcessingSnapshot` 1,800 B, `ProcessedValues` 96 B, result envelope 2,968 B,
`ModelProfileLifecycle` 11,952 B, `FisModelProfile` 1,416 B,
`SafetyModelProfile` 240 B, and `EncodedModelProfile` 1,464 B. The fixed
acquisition baseline remains bounded (`SampleRing<16>` 4,128 B); model and
result handoffs reject full pushes without overwriting older entries.
