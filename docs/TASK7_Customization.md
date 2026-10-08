# Task 7 network transport customization

Task 7 adds the bounded Wi-Fi/HTTPS delivery worker for the immutable archive
batch created by Task 6. It does not change the telemetry-v3 schemas, archive
bytes, or backend legacy `POST /api/ingest` route.

| Parameter | Owner and unit | Bounds/default | Apply policy |
| --- | --- | --- | --- |
| retry minimum/maximum | `AppConfig.telemetry`, milliseconds | validated `1..retry_max`, factory `1000..60000` | draft, validate, preview, commit, reboot |
| request timeout | `network::NetworkPolicy`, milliseconds | positive engineering policy, target default 10,000 | build policy; no hot reload |
| Retry-After ceiling | `network::NetworkPolicy`, milliseconds | positive, target default 60,000 | build policy |
| jitter ceiling | `network::NetworkPolicy`, milliseconds | bounded, target default 250 | build policy |
| archive endpoint | target network composition | HTTPS URL, bounded 192 bytes | build/provisioning boundary; never in archive batch |
| CA certificate | target network composition | required for TLS; no insecure fallback | build/provisioning boundary |
| Wi-Fi SSID/password | `SecretStore` | bounded secret blobs | USB write-only maintenance; loaded only by network owner |
| bearer device token | `SecretStore` | max 128 characters | USB write-only maintenance; never logged or batched |

`NetworkWorker` owns only a bounded token copy, retry state, and references to
the immutable batch source and transport. Acquisition, processing, and storage
continue when Wi-Fi is offline. A timeout, 429, or 5xx leaves exact batch bytes
pending and schedules bounded backoff. 401/403 requires operator credential
rotation; 409 and malformed ACKs are protocol faults and never advance the
local delivery journal.

The target HTTPS adapter requires an explicit CA certificate and endpoint. It
does not use insecure TLS, invent a production endpoint, or print credentials.
`POST /api/telemetry/batches` receives the exact durable body bytes. A valid
ACK must match device ID, batch ID, body hash, record count, and
`stored_count + deduplicated_count` before the Task 6 journal can checkpoint
and reclaim the source. BLE, HTTP workers for the legacy summary route, backend
archive code, migration 005, and actuator output remain outside Task 7.

Engineering values are not sensor calibration or scientific model values.
Calibration/model/capacity profiles remain typed versioned profiles, GPIO and
bus mapping remain build-time board policy, credentials remain in SecretStore,
and network runtime state is instance-owned RAM.
