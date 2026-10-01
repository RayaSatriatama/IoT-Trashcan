# Telemetry v3 contract boundary

This package is pure parsing, validation, canonicalization, identity, hash, ACK, and
retry-boundary logic. It intentionally has no `net/http`, SQLite, logger, profile store,
legacy normalize, fuzzy, or safety dependency. A later backend task can call it from a
transactional service after authenticating a Bearer token and before changing durable
state.

`ValidateBatch` rejects the whole request on the first structural or semantic defect.
`CheckRecordConflicts` is a preflight helper so a future transaction can detect a conflict
before inserting any record. It does not claim to provide storage atomicity by itself.
