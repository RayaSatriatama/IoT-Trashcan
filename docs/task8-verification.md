# Task 8 verification

This document records the backend telemetry-v3 archive slice. It is updated
with fresh command output before the Task 8 PR is reviewed.

## Implemented boundary

- `POST /api/telemetry/batches` validates the frozen v3 envelope before any
  write, authenticates the exact device, and commits manifest plus record rows
  transactionally.
- Exact request bytes and body hash are retained. Exact replay returns a
  durable deduplication ACK; a different body for the same batch identity is a
  conflict.
- `GET /api/telemetry/batches/{device_id}` lists bounded manifests and
  `GET /api/telemetry/batches/{device_id}/{batch_id...}` returns exact stored
  request bytes. Both read paths use the device credential boundary.
- Migration 005 is additive to the existing four migrations. Legacy ingest and
  dashboard read routes remain unchanged.

## Verification ledger

Results below were run on the Task 8 worktree after the archive and router
integration changes. Physical device commissioning, HTTPS delivery, backend
deployment, and MCU sensor evidence are outside this slice.

| Check | Result | Evidence |
| --- | --- | --- |
| Focused archive/http/database tests | PASS | `go test ./internal/telemetryarchive ./internal/http ./internal/database` |
| Full Go tests | PASS | `go test ./...` |
| `go vet ./...` | PASS | Go 1.27.1 |
| Schema and frozen fixture corpus | PASS | 14 schemas, 24 valid fixtures, 36 invalid fixtures |
| Record/body golden hashes | PASS | `a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883`; `3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c` |
| Runtime archive route | PASS | POST ingest, exact export, bounded manifest list, current projection, replay dedupe |
| Frontend/firmware regression | unchanged | Task 7/6 source was not modified by Task 8 |

The test database exercises migration 005, credential-bound ingestion,
transactional manifest and record persistence, exact replay bytes, dedupe and
conflict handling, all four record kinds, and additive route wiring. No
production server ACK or HTTPS worker is synthesized here; the ACK is produced
only after the local archive transaction commits.
