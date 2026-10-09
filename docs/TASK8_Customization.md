# Task 8 backend telemetry-v3 customization

Task 8 keeps telemetry-v3 archive policy separate from MCU configuration and
calibration profiles.

| Parameter | Type/unit | Default or bound | Owner | Apply policy |
| --- | --- | --- | --- | --- |
| telemetry body | bytes | `<= 32 KiB` | frozen v3 contract | request boundary |
| records per batch | count | `<= 128` | frozen v3 contract | request boundary |
| archive page size | count | 50 default, 200 maximum | `ArchiveQuery` | per read |
| record page size | count | 50 default, 200 maximum | `ArchiveQuery` | per read |
| record export body | bytes | streamed; does not buffer the complete archive | `ArchiveQuery`/HTTP read boundary | per request |
| device credential | secret bytes | existing `SecretStore`/digest | device auth | before archive mutation |
| exact batch bytes | SQLite BLOB | one immutable body per `(device_id,batch_id)` | migration 005 | committed transaction |
| current candidate projection | one row per source/boot | durable SQLite trigger projection; full archive remains authoritative | migration 005 | transactionally updated with record insert |

Migration `005_telemetry_archive.sql` is additive. It stores the exact request
body, durable batch manifest, record identity/hash bindings, and exact record
bytes. The legacy `/api/ingest` summary-v2 path remains unchanged.

The endpoint authenticates the batch `device_id` against the existing device
credential digest. An identical batch is acknowledged as deduplicated. Reusing
a batch ID with another body hash is a conflict and cannot partially mutate the
archive. Archive export returns stored bytes; it never rebuilds a body with a
new serializer. No MCU FIS/Safety recalculation or network worker is added in
Task 8.

Read surfaces are bounded and use the same device credential boundary:
manifest listing, exact batch export, record-level archive export, and a
current projection. Exact batch and record export return stored evidence; they
never rebuild a body with a new serializer or recalculate an MCU model
decision.

Current-state ordering is based on trusted acquisition chronology, not server
insert time. Same-boot `sequence`/`monotonic_us` order is valid. Monotonic
values from different boots are never compared as one timeline. UTC is used
only when both competing records have trusted UTC evidence; if either side has
untrusted or missing UTC, the projection reports unresolved ordering instead
of inventing a timestamp. A late offline batch therefore cannot replace newer
current evidence.

Current samples are keyed by both stream and sensor identity. For example,
`hx711_organic`, `ultrasonic_organic`, `mq4`, `mq135`, and `sht31_44` remain
independent entries under `organic`; one sensor cannot overwrite another.
Results remain independently keyed by stream. Quality, reason, raw/converted
nullability, provenance, source IDs, and original record identity are retained.

Current reads use `telemetry_current_candidates`, an indexed durable projection
containing only the newest sequence/monotonic candidate for each
`(device, kind, stream, sensor, boot)`. This removes the old global candidate
scan without dropping archival rows. Cross-boot comparison still occurs in the
read layer because monotonic time is not comparable across boots and missing or
untrusted UTC remains explicitly unresolved. The projection is updated by an
SQLite trigger in the same ingest transaction and may be rebuilt from the
authoritative record table if recovery tooling requires it.

Synthetic benchmark on the repository test SQLite backend (AMD Ryzen 3 5300U,
Go 1.27.1, one benchmark iteration) measured current reads over 100, 1,000,
and 10,000 retained records for one source/boot at approximately 177 us,
84 us, and 96 us respectively. The 1,000 and 10,000 cases each used 8.3 KiB
and 43 allocations per read, demonstrating that read cost follows the bounded
candidate projection rather than total retained history. These are host-side
measurements, not production database latency or hardware endurance claims.

Record reads are exposed at `/api/telemetry/records/{device_id}` and
`/api/telemetry/records/{device_id}/export`. They use a stable decimal-uint64
cursor derived from `(device_id, boot_id, sequence, record_id)`. The extra
`record_id` tie-breaker keeps pagination stable if several record kinds share a
sequence. Reads support sensor, stream, record-kind, quality, and UTC-null
filters. SQLite integer columns are not used for raw uint64 sequence values;
the canonical decimal representation and a lossless sort key are retained.

Migration 005 remains additive and is the source of truth for exact batch
bytes and record bindings. Any additive record-index/backfill change must be
safe for existing rows and must preserve legacy `/api/ingest` behavior.

The backend does not recalculate hardware-v3 normalization, FIS, or Safety.
It stores and projects the MCU decisions and provenance as received. Device
credentials remain in the authentication boundary and are never returned to
the frontend.
