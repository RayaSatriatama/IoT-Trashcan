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

## Remediation acceptance scope

The archive read boundary is also required to provide a complete record-level
view. The current-state projection compares trusted acquisition chronology,
not insertion time: same-boot sequence/monotonic order is usable, while
cross-boot monotonic values and untrusted or absent UTC remain explicitly
unresolved. A late offline backlog must not replace newer current evidence.
Samples are projected by `(stream, sensor_id)` so independent sensors sharing a
stream are retained. Results remain independently projected by stream.

Current reads use the durable `telemetry_current_candidates` projection, with
one candidate per source and boot maintained by the migration-005 insert
trigger. The complete `telemetry_records` table remains authoritative. The
projection stores raw record bytes and source chronology metadata, so the read
path no longer decodes every historical record on each request. Decimal-string
monotonic comparison is length-aware and therefore remains correct at digit
boundaries.

Record export is authenticated and bounded. It uses a stable cursor composed
from canonical `device_id`, `boot_id`, and decimal uint64 `sequence`, supports
sensor/stream/kind/quality/UTC-null filters, and streams pages without loading
the complete archive into memory. Exact stored evidence and provenance are
returned; no hardware-v3 normalization, FIS, or Safety decisions are
recalculated by the backend.

The remediation tests must cover same-boot ordering, cross-boot chronology,
UTC-null records, late backfill, more than 200 records for one sensor,
same-stream multi-sensor projection, maximum uint64 cursor values, stable
pagination, authorization, and exact record evidence. Results below are only
marked PASS after those tests execute on the final HEAD.

## Verification ledger

Results below were rerun after the archive projection and record-read
remediation. Physical device commissioning, HTTPS delivery, backend
deployment, and MCU sensor evidence are outside this slice.

| Check | Result | Evidence |
| --- | --- | --- |
| Focused archive/http/database tests | PASS | `PATH=/home/raya/.local/opt/go1.27.1/bin:$PATH go test -count=1 ./internal/telemetryarchive ./internal/http ./internal/database`; record cursor/export, chronology, per-sensor, authenticated route, production composition, and malformed export cursor regressions |
| Full Go tests | PASS | `PATH=/home/raya/.local/opt/go1.27.1/bin:$PATH go test -count=1 ./...` |
| Go race tests | PASS | `PATH=/home/raya/.local/opt/go1.27.1/bin:$PATH go test -race -count=1 ./...` |
| `go vet ./...` | PASS | `PATH=/home/raya/.local/opt/go1.27.1/bin:$PATH go vet ./...`; Go 1.27.1 |
| Schema and frozen fixture corpus | PASS | 14 schemas compile; 24 valid fixtures pass semantic schema validation; 36 invalid fixtures reject as expected |
| Record/body golden hashes | PASS | `a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883`; `3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c` |
| Runtime archive route | PASS | POST ingest, exact batch replay, bounded manifest list, materialized per-sensor current projection, chronology-safe late-backfill handling, authenticated record page/export, exact raw-record preservation, and replay dedupe |
| Current projection benchmark | PASS | Host SQLite benchmark, 100 measured iterations: 100 records 21.4 us / 1,923 B / 32 allocs; 1,000 records 21.8 us / 1,976 B / 32 allocs; 10,000 records 20.7 us / 1,976 B / 32 allocs |
| Firmware native suite | PASS | 365/365 on Task 8 worktree |
| Firmware ASan/UBSan suite | PASS | 365/365 with address/undefined sanitizers |
| Firmware cppcheck | PASS | no defects |
| Firmware targets | PASS | `sigap_n16r8`, `sigap_n16r8_sd`, `sigap_n16r8_sd_spi`, `sigap_bench` |
| Frontend/firmware regression | unchanged source | Task 7/6 firmware source was not modified by Task 8 |

The fresh target size probes are unchanged from the Task 7 merge because this
branch only adds Go/migration/HTTP archive code: `sigap_n16r8` and `sigap_bench`
use 49,496 B RAM and 1,073,557 B flash; `sigap_n16r8_sd` uses 50,036 B RAM and
1,147,337 B flash; `sigap_n16r8_sd_spi` uses 49,792 B RAM and 1,126,773 B flash.

The test database exercises migration 005, credential-bound ingestion,
transactional manifest and record persistence, exact replay bytes, dedupe and
conflict handling, all four record kinds, and additive route wiring. No
production server ACK or HTTPS worker is synthesized here; the ACK is produced
only after the local archive transaction commits.

The record-level archive index is additive to unreleased migration 005 and
stores canonical decimal sequence text plus a 20-character lossless sort key;
SQLite signed integers are never used for uint64 sequence ordering. The
materialized current projection retains one candidate per source and boot, so
a high-rate source cannot hide a sparse source behind an arbitrary recent-row
limit. When chronology cannot be established across boots because trusted UTC
is absent, `ordering_unknown` is returned rather than inventing a total order.
Record export writes the persisted record JSON bytes directly inside its
documented metadata envelope; whitespace, Unicode, escapes, and HTML-sensitive
characters are covered by byte-level regression tests.

The backend change does not modify firmware source, telemetry schemas, or
frozen canonical/body hashes. Existing firmware evidence therefore remains
applicable, but it is not repeated as a new firmware run for this Go-only
remediation.
