# Task 7 plan: bounded Wi-Fi/HTTPS transport

Authority: PRD Firmware SIGAP v1.3, TSD SIGAP v1.2 sections 9 and 10B, and
the frozen telemetry-v3 schemas/ACK fixtures.

## Traceability

| Requirement | Implementation seam | Evidence |
| --- | --- | --- |
| Wi-Fi loss does not stop acquisition/storage | `network::NetworkWorker` polls a bounded link seam after archive work | offline native regression; target compile |
| Exact immutable retry bytes | `IImmutableBatchSource` exposes `ImmutableBatch::exact_body()` | byte-for-byte retry test |
| Backoff and Retry-After bounds | `NetworkPolicy` and monotonic retry deadline | timeout, 429, 5xx, jitter tests |
| HTTPS and bearer secret boundary | target `WiFiArchiveHttpTransport` plus `SecretStore` load in composition | request-boundary tests; target build |
| ACK is authoritative | `ack_matches` and Task 6 journal/checkpoint path | malformed/mismatch/409/replay tests |
| No insecure or synthetic delivery | CA-required target adapter; no production ACK generation | code review and target build |
| Network worker is off sensor timing path | worker is polled after acquisition, processing, and storage | composition review and runtime test |

BLE gateway, legacy summary transport, backend archive, migration 005, and
actuator output are explicitly deferred.
