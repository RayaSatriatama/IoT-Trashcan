# Implementation Plan: SIGAP Telemetry v3 Contract Freeze

## Baseline and scope

- Baseline: `main` after the approved Task 2 merge, `c07f512f7592a3427f5f659766c6dba1fdcae97b`.
- Scope: additive archive-v3 contract under `contracts/telemetry/v3/`, strict Go parser/validation/hash helpers, firmware contract boundary and fixtures, ADRs, PRD/TSD reconciliation, and contract-focused tests.
- Explicitly deferred: HTTP handler, SQLite migration, durable archive, logger/journal, FIS/safety implementation, profile persistence, and Task 4.

## Architecture decisions

- Preserve `POST /api/ingest` as summary-v2; v3 is a separate contract boundary.
- Use RFC 8785 JSON Canonicalization Scheme (JCS) for record hashes. Batch hashes use exact request bytes and are never included in the body.
- Represent every wire uint64 as canonical decimal strings; provide a 20-digit sort key helper for future SQLite storage.
- Require immutable record identity and `sha256:` lowercase record hashes on every v3 record.
- Reject duplicate keys, malformed UTF-8, unsupported schema, unknown enums, invalid identities, non-finite numbers, and all resource-limit violations before semantic acceptance.
- Keep Go v3 validation independent from legacy ingest/fuzzy/safety packages. Firmware exposes typed bounded contract primitives without coupling acquisition structs to JSON.

## Task list

### Phase 1: Contract and fixture freeze

- [ ] Audit PRD/TSD and existing Go/firmware conventions; document contradictions and the v3 capability map.
- [ ] Add v3 schemas, README, ADRs, valid/invalid fixture corpus, and exact canonical/body-hash fixtures.

### Phase 2: Go contract boundary

- [ ] Add strict parser, duplicate-key/UTF-8/depth/size checks, canonical uint64/identity/hash helpers, typed batch/record/ACK DTOs, and atomic conflict semantics as pure validation outcomes.
- [ ] Add focused tests for valid/invalid fixtures, JCS hashes, exact body hashes, ACK validation, identity/conflict rules, and legacy-package isolation.

### Phase 3: Firmware contract boundary

- [ ] Add bounded C++ uint64/identity/hash fixture helpers and cross-language canonical/hash tests without coupling to acquisition or adding telemetry transport.
- [ ] Verify Task 2 firmware tests/builds remain unchanged and resource impact is bounded.

### Checkpoint: contract freeze

- [ ] Go and native contract tests pass.
- [ ] Cross-language canonical/hash and exact body-byte fixtures agree.
- [ ] Existing legacy tests and `POST /api/ingest` code are unchanged.
- [ ] No migration 005, HTTP handler, archive persistence, or Task 4 behavior is added.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| JCS number formatting differs across languages | Use an RFC 8785 implementation/algorithm, committed canonical-byte fixtures, and independent expected digests. |
| Strict duplicate-key rejection is bypassed by `encoding/json` | Scan JSON tokens recursively before typed decode and reject duplicates per object. |
| Contract implementation leaks legacy inference | Keep package imports and tests separate; add static import guard. |
| Fixture payloads exceed frozen limits | Measure serialized bytes and retain source IDs/raw/trace; report any impossible envelope rather than truncating. |
| New contract is mistaken for an endpoint implementation | Document additive route and explicitly defer HTTP/database work. |

## Traceability targets

- PRD v1.3 §§2A, 9, 11, 12/T0, AC08-AC09, AC12-AC16, AC22-AC25, AC27-AC35.
- TSD v1.2 §§2, 4, 6-7, 9.2, 10-12, 13-16.
