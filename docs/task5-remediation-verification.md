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

## Fresh verification record at 300d09e

The reviewer follow-up was synchronized and verified from source commit
`300d09ec9d2a7723af453e2a8631432155a80723`:

- Native PlatformIO tests: **241/241 passed**, including both retained-cache
  freshness regressions.
- Native ASan/UBSan copy: **241/241 passed** with no sanitizer diagnostics.
- Cppcheck: **No defects found** with `--fail-on-defect=low`.
- Target builds: `sigap_n16r8`, `sigap_n16r8_sd`, and `sigap_bench` all passed.
  Each measured 77,680 B RAM / 400,509 B flash.
- Go 1.27.1 isolated toolchain: telemetry tests, full `go test ./...`, and
  `go vet ./...` passed.
- Schema conformance: all 14 Draft 2020-12 schemas compiled; 24 valid fixtures
  passed schema validation and 36 invalid fixtures retained expected Go
  rejection codes. Canonical record and exact-body golden hashes passed.
- `git diff --check`: clean.
- Physical HIL: not required and not performed; no sensor/output wiring exists.

## Resource notes

Final target footprint is **77,680 B RAM / 400,509 B flash** for each of the
three profiles. Native measured sizes are: `ProcessingRuntime` 16,728 B,
`ProcessingSnapshot` 1,944 B, `ProcessedValues` 96 B, result envelope 2,968 B,
`ModelProfileLifecycle` 11,952 B, `ProfileManager` 10,120 B,
`OperationalCapacityProfile` 200 B, `ProfileManifest` 184 B,
`EncodedProfile` 192 B, and `SampleRing<16>` 4,128 B. Acquisition remains
bounded (`SampleRecord` 256 B, `AcquisitionFoundation` 8,888 B,
`TargetAcquisitionGraph` 4,408 B); model, sample, and result handoffs reject
full pushes without overwriting older entries.

The reviewer follow-up adds two freshness regressions over the prior 239-case
checkpoint and retains the fixed 13-channel processing budget. No target heap
high-water or sensor timing claim is made because no sensors or outputs are
connected.


## Independent review follow-up: retained channel freshness

Review reproduced a remaining cadence regression: an SHT31 sample at 900,000 us
was valid at the 1,000,000 us cycle but became Missing at 2,000,000 us despite
being only 1.1 seconds old (the initial freshness bound is 5 seconds).
The latest-per-channel cache now survives processing cycles. begin/disable
still clears it, and every cycle rechecks age, quality, and profile identity.
A newer failed attempt still supersedes a retained READY attempt.

- Added two ProcessingRuntime regressions for between-cycle freshness, stale
  expiry, source provenance, and newest timeout precedence.
- Before the fix, the runtime suite had 10 tests with 1 freshness failure.
- After the fix, 89 relevant existing/new native tests across 9 suites passed
  using GCC C++17 with ASan/UBSan (LeakSanitizer disabled in the review environment).
- Production change adds no fields or dynamic allocation. Native ProcessingRuntime
  footprint remains 16,728 bytes.
- The reviewer-only 89-test ASan result is historical for diagnosis; the full
  241-case PlatformIO ASan/UBSan run above is the merge-gate evidence.
- No hardware upload, reboot, unplug, SD access, sensor, or actuator HIL performed.
