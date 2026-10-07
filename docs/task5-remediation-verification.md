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
| Independent processing cadence and bounded retention | `processing_policy::kProcessingPeriodUs`, latest-per-channel cache, current-time late poll | cadence, 13-channel/20-round retention, and late-poll tests |
| Mechanical capacity provenance | `OperationalCapacityProfile`, typed codec, ProfileManager A/B binding, runtime snapshot | codec/lifecycle/reload and utilization snapshot tests |
| Model import ownership | validation-before-stage and router-owned draft/transfer state | wrong hash/target, competing export, and explicit activation tests |
| Safety/unit semantics | MQ7/MQ8 hazard mapping, B3 evaluator isolation, model input-unit mapping | pipeline latch and MQ hazard tests; all five input-unit mismatch cases |

## Policy boundaries

AppConfig schema v1 and factory defaults are unchanged. Sampling and telemetry
cadence remain AppConfig policy. Calibration and scientific values remain typed
versioned profiles. Board pins and protocol constants remain build-time. Secrets
remain in SecretStore. Runtime state, safety latch, windows, and handoff rings
are instance-owned RAM state. TEST_ONLY model fixtures are never installed by
target composition.

The processing cycle is intentionally independent of `telemetry.summary_period_ms`:
the bounded engineering policy is one cycle per second. The window retains one
newest attempt for each of the 13 current acquisition channels within 16 fixed
records and forwards every observed sample to the downstream logger observer.
Capacity profiles use the existing typed ProfileManager and pointer-last store;
no load-cell rating is used as an operational default.

## Verification record

Fresh verification on the integrated source completed as follows:

- Native PlatformIO tests: **239/239 passed**.
- Native ASan/UBSan PlatformIO copy: **239/239 passed**.
- Cppcheck: **No defects found** with `--fail-on-defect=low`.
- Target builds: `sigap_n16r8`, `sigap_n16r8_sd`, and `sigap_bench` all passed.
- Go 1.27.1 isolated toolchain: telemetry tests, full `go test ./...`, and
  `go vet ./...` passed.
- Schema conformance: all 14 Draft 2020-12 schemas compiled; 24 valid fixtures
  passed schema validation and 36 invalid fixtures retained expected Go
  rejection codes. Frozen record and exact-body hash tests passed.
- Physical HIL: not required and not performed; no sensor/output wiring exists.

## Resource notes

Final target footprint is **77,680 B RAM / 400,517 B flash** for each of the
three profiles. Native measured sizes are: `ProcessingRuntime` 16,728 B,
`ProcessingSnapshot` 1,944 B, `ProcessedValues` 96 B, result envelope 2,968 B,
`ModelProfileLifecycle` 11,952 B, `ProfileManager` 10,120 B,
`OperationalCapacityProfile` 200 B, `ProfileManifest` 184 B,
`EncodedProfile` 192 B, and `SampleRing<16>` 4,128 B. Acquisition remains
bounded (`SampleRecord` 256 B, `AcquisitionFoundation` 8,888 B,
`TargetAcquisitionGraph` 4,408 B); model, sample, and result handoffs reject
full pushes without overwriting older entries.

The remediation adds 11 native regression cases over the previous checkpoint
and retains the fixed 13-channel processing budget. No target heap high-water
or sensor timing claim is made because no sensors or outputs are connected.
