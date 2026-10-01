# ADR: Telemetry v3 contract and identity freeze

## Status

Accepted for Task 3 contract freeze. HTTP, persistence, and profile maintenance remain deferred.

## Context

SIGAP needs a complete hardware-edge archive without changing the legacy summary-v2
ingest path. Retries, duplicate delivery, JavaScript number limits, and heterogeneous
Go/C++ serializers make implicit JSON equality unsafe.

## Decisions

1. Archive v3 uses an additive batch shape with `samples`, `results`, `events`, and
   `metadata`; the legacy `manifest_ids` proposal is removed because profile references
   are provenance objects and the server can derive record IDs from arrays.
2. Every record uses `<device_id>/<boot_id>/<sequence>` identity and a required
   `sha256:` record hash over the complete record after removing only `record_hash`.
   RFC 8785 JCS canonical bytes are the cross-language hash input.
3. Every wire uint64 is a canonical decimal string. A 20-digit zero-padded key is a
   derived future SQLite ordering value, never a second wire identity.
4. Batch equality hashes exact HTTP body bytes. Body hash is external metadata and is
   never inserted into the body. After first send, body bytes and batch ID are immutable.
5. Duplicate object keys, invalid UTF-8, non-finite numbers, unknown fields/enums,
   inconsistent identities, and limit violations are rejected before semantic use.
6. The initial ceilings are 32 KiB body, 16 KiB record, 128 records, depth 12, and
   512 UTF-8 bytes per free-form string. Raw evidence and source IDs are never truncated.
7. Same batch identity plus same body hash is replay-safe. Different body hash or a
   record identity with a different record hash is a whole-batch conflict (HTTP 409).
   No partial commit is part of this contract.
8. `profile_refs` and `config_ref` carry non-secret provenance only. A missing manifest
   does not invalidate a structurally valid raw record. Credentials remain SecretStore data.
9. Go validation is a separate package with no imports of legacy normalize, fuzzy, or
   safety code. Firmware receives a typed bounded boundary and does not serialize its
   acquisition structs directly.

## Consequences

The contract can be tested before an HTTP handler or migration exists. Exact retry bytes,
canonical record hashes, and conflict rules are stable for later Task 4 and backend work.
The canonicalization dependency is pinned and audited; future changes require a new schema
version or an explicit ADR.
