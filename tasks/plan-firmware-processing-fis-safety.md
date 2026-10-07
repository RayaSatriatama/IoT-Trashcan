# Implementation Plan: SIGAP Task 5 Processing, Models, FIS, and Safety

## Baseline and scope

- Task 4 merge SHA: `2d886d6dc0843bd932a1da73eb233713c559d032`.
- Branch: `feat/firmware-processing-foundation`, based directly on fresh `main`.
- Scope: typed model profiles, bounded sample processing, configurable TEST_ONLY Mamdani FIS, separate B3 safety state machine, immutable cycle snapshots, provenance, and native verification.
- Explicitly out of scope: durable logger, SD/LittleFS archive, network workers, HTTPS, backend archive, migration 005, actuator output, and physical HIL. No sensors or outputs are connected.

## Authority and constraints

- PRD Firmware SIGAP v1.3, TSD Firmware SIGAP v1.2, and frozen telemetry v3 contracts are authoritative.
- Existing AppConfig schema and ProfileManager/profile-store boundaries remain unchanged.
- No production scientific thresholds, gas ppm/R0/OGI, or fabricated sensor readings.
- Every implementation slice includes native tests; no test-only shortcut may be enabled in runtime composition.
- Fixed-capacity containers and bounded serialization are preferred. Active model/profile changes are draft/validate/preview/commit and apply at reboot or cycle boundary as defined by ownership.

## Shared contracts before parallel implementation

1. Define a bounded processing/result interface that carries source sample IDs, immutable config/profile refs, quality/reason, and separate FIS/Safety results. [done]
2. Define typed model profile identity, validation, and codec boundary reusing existing profile persistence primitives without duplicating ProfileManager. [done]
3. Record ownership and compatibility notes in a progress ledger before dispatch. [done]

## Task list

### Phase 1: Shared foundation

- [x] Task 1: Shared model/result contracts and provenance boundaries.
  - Acceptance: typed bounded interfaces compile natively, preserve config/profile identity, and do not change AppConfig schema.
  - Verification: focused native contract tests and compile.
  - Dependencies: none.

### Phase 2: Independent feature slices

- [x] Task 2: Versioned model profiles, validation, deterministic codec, and lifecycle tests.
  - Acceptance: FIS and Safety model profiles validate finite domains/rules, encode deterministically, and use existing A/B fault-injection boundary.
  - Verification: lifecycle, interrupted commit, hash/version, invalid-profile tests.
  - Dependencies: Task 1.

- [x] Task 3: Processing window, freshness/quality selection, conversion/normalization, source IDs, and result snapshot tests.
  - Acceptance: only real eligible samples contribute; stale/missing/calibrating inputs remain unavailable; cycle snapshots cannot mix profile/config versions.
  - Verification: deterministic window, source-ID, provenance, invalid-input, and bounded-handoff tests.
  - Dependencies: Task 1.

- [x] Task 4: Bounded configurable Mamdani FIS evaluator and audit trace.
  - Acceptance: tri/trap memberships, stable rules, min/max/clipping/aggregation/centroid, finite/domain/coverage/trace bounds, and TEST_ONLY bench fixture.
  - Verification: boundary memberships, independent centroid fixture, missing/zero-area/invalid-model/trace-overflow tests.
  - Dependencies: Task 1 and Task 2 model contract.

- [x] Task 5: Separate B3 safety model/state machine.
  - Acceptance: invalid hazard input yields UNAVAILABLE/FAULT, alert latch/clear obeys validated rules, collection score cannot clear alert, and no production thresholds are invented.
  - Verification: state transition, hysteresis/latch/clear, invalid-input, and profile lifecycle tests.
  - Dependencies: Task 1 and Task 2 model contract.

### Checkpoint: integrated processing edge

- [x] Compose processing, FIS, and Safety with immutable cycle snapshots.
- [x] Verify source IDs/config refs/profile refs and v3-compatible result shapes.
- [x] Run native suite, sanitizers, cppcheck, and three PlatformIO targets.

### Phase 3: Documentation and publication

- [x] Document customization parameters, defaults, units, ranges, authority, and apply policy in `docs/`.
- [x] Add traceability/evidence report and update PR description.
- [x] Push branch and open Task 5 PR without merging.

## Remediation checkpoint

- ProcessingRuntime is application-owned and polls bounded cycles without
  consuming the acquisition handoff used by future logging.
- Model profiles use independent typed keys with target NVS A/B persistence,
  pointer-last activation, rollback, and explicit USB staging/validation/
  preview/commit. TEST_ONLY remains a native fixture state.
- The newest matching HX711 attempt gates operational readiness, including
  CALIBRATING, timeout, and disconnected attempts.
- SafetyEvaluator is instance-owned by the runtime. B3 alerts remain latched
  until validated consecutive clear cycles and cannot be cleared by model
  replacement or invalid input.
- Cycle snapshots copy config, calibration, and model identities. Source IDs
  are checked against the result device/boot context, and conflicting profile
  identities are rejected.
- MQ7/MQ8 model inputs consume only valid ADC millivolt evidence with matching
  profile provenance. No ppm, R0, OGI, or production threshold is inferred.

## Integration decisions

- Processing owns sample eligibility, conversion, normalization, and result provenance.
- FIS and Safety own their distinct typed result vocabularies and traces; no shared generic model-result state enum.
- Model profiles are immutable snapshots per processing cycle. A committed profile becomes visible to new cycles according to the existing activation policy; an in-flight cycle keeps its captured refs.
- TEST_ONLY profiles are accepted only in explicit native bench fixtures and are never installed by target composition.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Existing acquisition types do not expose enough typed evidence | Build adapters around public `SampleRecord`/profile refs; leave unsupported values null rather than fabricate. |
| Profile codec boundary is narrower than model payload | Use bounded model payload and existing encoded-profile integrity semantics; reject oversize profiles. |
| FIS trace exceeds fixed memory | Validate rule/input/grid limits before evaluation and return bounded unavailable result. |
| Safety semantics accidentally reuse FIS vocabulary | Separate headers, enums, validators, and tests. |

## Open questions resolved for this slice

- No production model thresholds are available, so only explicit TEST_ONLY fixtures are evaluated as ready.
- No HIL is required for processing-only deterministic tests because sensors and outputs are absent; target build evidence remains separate from physical validation.
