# v3 boundary errors and HTTP mapping

The pure contract package returns stable categories. A future handler may map them as
follows without changing the parser:

| Category | HTTP mapping |
| --- | --- |
| `MALFORMED_JSON`, `DUPLICATE_KEY`, `SCHEMA_UNSUPPORTED`, `INVALID_UINT64`, `INVALID_IDENTITY`, `INVALID_ENUM`, `COUNT_MISMATCH`, `HASH_MISMATCH`, `RECORD_TOO_LARGE`, `BODY_TOO_LARGE`, `DEPTH_EXCEEDED`, `STRING_TOO_LONG`, `PROFILE_REF_INVALID`, `SOURCE_SAMPLE_IDS_MISSING` | 400 Bad Request |
| device token absent or invalid | 401 Unauthorized |
| valid token bound to another device | 403 Forbidden |
| unknown device or target | 404 Not Found |
| batch or record identity conflict | 409 Conflict; reject the whole batch |
| unsupported content type | 415 Unsupported Media Type |
| rate limit | 429 Too Many Requests |
| backend/storage failure | 5xx; no durable ACK |

The handler is deliberately out of scope for Task 3. Authentication uses
`Authorization: Bearer`; credentials never appear in JSON fixtures or body hashes.
