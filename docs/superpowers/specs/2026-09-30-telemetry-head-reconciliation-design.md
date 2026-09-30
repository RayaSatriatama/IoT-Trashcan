# SIGAP Telemetry HEAD Reconciliation Design

**Date:** 2026-09-30

## Purpose

Reconcile the ESP32-S3 firmware PRD and TSD, authored against historical
baseline `6fed00f02b6ef7dabc159d924f798d344a6072ba`, with the current
repository state at `ad65d5fe73c30b16c61da917eb517cdf2a783ba6`. The outcome
must give a future telemetry implementation an explicit compatibility contract
without rewriting deployed history or changing current runtime behavior.

## Scope and non-goals

In scope:

- Document the impact of commits `98e4439`, `292aa47`, and `ad65d5f`.
- Update firmware Markdown documents to distinguish historical evidence from
  the current integration target.
- Keep the generated PRD DOCX aligned with its Markdown source.
- Strengthen the database migration regression test for the existing `004`
  migration.

Out of scope:

- Adding PlatformIO firmware, a telemetry batch endpoint, or telemetry tables.
- Modifying `POST /api/ingest`, existing dashboard routes, current simulator
  behavior, or current SQLite data.
- Reverting, rebasing, or amending commits already merged to `main`.

## Reconciliation evidence

`6fed00f` is an ancestor of `main`; `main` is three commits ahead:

| Commit | Classification | Required handling |
| --- | --- | --- |
| `98e4439` | Runtime and schema change | Preserve its ingest, fuzzy, safety, history, read API, simulator, and research-trace behavior as the integration surface. |
| `292aa47` | Firmware documentation addition | Correct the new PRD and TSD with this reconciliation record. |
| `ad65d5f` | Project skill assets only | Record as non-runtime context; it imposes no firmware, API, or schema compatibility change. |

Commit `98e4439` added `migrations/004_device_research_trace.sql`, which adds
`trace_json` and `safety_json` to `decisions`. The read research DTO consumes
both fields. A future telemetry archive must therefore be additive and must not
reuse, overwrite, or reinterpret those columns.

## Compatibility decisions

### Historical baseline and current target

The PRD and TSD retain `6fed00f` as the historical source for observations
made before the monitoring release. They must also name `ad65d5f` as the
reconciliation target and state that future implementation starts by checking
the then-current `main` again.

### Ingest separation

`POST /api/ingest` remains the compatible summary-v2 route. It accepts the
existing authenticated three-slot request and preserves exact raw-payload replay
semantics. A telemetry v3 batch transport is a new route and handler; it must
not change the request, response, or behavior of the existing route.

The batch path validates and persists device-produced edge results. It does not
invoke the legacy normalize, fuzzy, or safety pipeline for hardware v2/v3
records. Existing simulator and legacy ingest paths continue to own their
current evaluation behavior.

### Migration sequencing

The current migration set ends at
`004_device_research_trace.sql`. The database migrator embeds every SQL file
and applies names in lexicographic order. Consequently, the expected telemetry
archive filename at this reconciliation point is
`005_telemetry_archive.sql`.

The number is not reserved by this document. The implementation PR must inspect
the migration directory immediately before creating its SQL file. If another
migration has landed first, the archive migration uses the next free ordered
number instead.

### Research and history compatibility

The existing history API uses a bounded cursor based on
`(recorded_at, reading_id)`, while telemetry archive records require a stable
device sequence identity `(device_id, boot_id, sequence)` and may have null
UTC. Archive queries, exports, and DTOs are additive. They cannot silently
change dashboard history ordering, existing CSV columns, or the semantics of
current current-status reads.

## Required document changes

1. Add a dated reconciliation section to the TSD after the baseline section.
2. Amend the TSD target tree to identify `005_telemetry_archive.sql` as the
   current expected name and retain the pre-implementation verification rule.
3. Add the same historical-baseline/current-target distinction and compatibility
   rules to the PRD.
4. Regenerate the PRD DOCX from the corrected Markdown and verify that its
   visible content names the same baseline, target, migration rule, and route
   separation.

## Required test correction

`TestOpenMigrationIsIdempotent` currently only checks migrations `001` through
`003`, despite `004` being embedded and applied. Update it to assert all four
current migration names and to verify that `decisions` exposes `trace_json` and
`safety_json` after opening a fresh database. This is a regression test for the
already-shipped migration, not a schema change.

## Acceptance criteria

- Both Markdown firmware documents name `6fed00f` as historical baseline and
  `ad65d5f` as the reconciliation target.
- Both documents classify all three commits and preserve the existing summary
  ingest route.
- Telemetry archive documentation identifies `005` as the current expected
  next migration and requires an execution-time recheck.
- The PRD DOCX is regenerated from the corrected Markdown.
- Database migration tests prove that `004` is applied and its two decision
  columns exist.
- No runtime endpoint, schema migration, simulator behavior, or application
  dependency changes in this reconciliation PR.
