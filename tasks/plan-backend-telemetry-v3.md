# Task 8 backend telemetry-v3 plan

## Shared contract

- `POST /api/telemetry/batches` is additive. `/api/ingest` remains unchanged.
- The request body is bounded by telemetry-v3 (`32 KiB`, `128` records) and is
  validated before any archive mutation.
- Device authentication uses the existing `devices.credential_digest` and the
  exact `device_id` from the v3 envelope. No token is stored in archive rows.
- The exact request bytes and body hash are durable. A repeated identical
  `(device_id, batch_id, body_hash)` is deduplicated; a batch-id conflict with a
  different hash is rejected transactionally.
- Record archive rows retain record identity, record hash, kind, and exact
  record JSON. Archive reads never rebuild a request body through a serializer.
- A durable ACK is returned only after the batch manifest and record rows commit.

## Traceability

| Requirement | Implementation | Evidence |
| --- | --- | --- |
| Additive batch endpoint | `internal/http/telemetry_batch_handler.go` | handler tests |
| Transactional archive | `internal/telemetryarchive/service.go`, migration 005 | service tests |
| Exact bytes and dedupe/conflict | manifest/body hash constraints | round-trip tests |
| Archive query/export | `internal/telemetryarchive/query.go` | query tests |
| Migration sequencing | `migrations/005_telemetry_archive.sql` | database tests |
| Legacy compatibility | existing ingest route untouched | full Go suite |

## Out of scope

HTTPS worker changes, backend migration 005 beyond the archive schema, MCU
FIS/Safety recalculation, network transport, durable logger changes, and Task 9.
