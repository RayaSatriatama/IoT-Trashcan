# Task 8 backend telemetry-v3 customization

Task 8 keeps telemetry-v3 archive policy separate from MCU configuration and
calibration profiles.

| Parameter | Type/unit | Default or bound | Owner | Apply policy |
| --- | --- | --- | --- | --- |
| telemetry body | bytes | `<= 32 KiB` | frozen v3 contract | request boundary |
| records per batch | count | `<= 128` | frozen v3 contract | request boundary |
| archive page size | count | 50 default, 200 maximum | `ArchiveQuery` | per read |
| device credential | secret bytes | existing `SecretStore`/digest | device auth | before archive mutation |
| exact batch bytes | SQLite BLOB | one immutable body per `(device_id,batch_id)` | migration 005 | committed transaction |

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
manifest listing, exact batch export, and a current projection containing the
newest persisted sample/result JSON per stream. The projection returns stored
evidence and does not recalculate a model decision.
