# Telemetry v3 Contract Remediation Plan

## Objective

Complete the Task 3 contract freeze beyond envelope validation by freezing sensor,
result, event, and metadata payloads, making the fixture corpus executable, and
proving Go and firmware canonical bytes independently.

## Work slices

1. Sensor payloads and sensor-to-stream identity mapping.
2. Typed result, FIS trace, safety trace, event, and metadata payloads.
3. ACK arithmetic, strict hash entrypoint, parser bounds, and manifest-driven fixtures.
4. Bounded C++ semantic canonical writer and cross-language byte/hash fixture.
5. PRD/TSD and ADR reconciliation, then integrated verification.

## Acceptance criteria

- [ ] Every sensor family has a closed raw/converted payload shape.
- [ ] Sensor-to-stream combinations, including SHT31 0x44 and 0x45, are enforced.
- [ ] Result, event, and metadata payloads reject unknown nested fields.
- [ ] Every committed valid and invalid fixture is executed with exact expected results.
- [ ] ACK count arithmetic cannot wrap.
- [ ] Nested parser limits do not alias the 128-record batch limit.
- [ ] C++ constructs canonical fixture bytes from typed semantic fields.
- [ ] Go, firmware native, static, target, and diff checks pass.

## Scope guard

No HTTP handler, migration 005, archive persistence, logger, FIS engine, safety
engine, profile persistence, or Task 4 implementation belongs in this remediation.
