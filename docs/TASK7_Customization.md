# Task 7 customization and ownership

Task 7 adds a bounded Wi-Fi station manager, always-on encrypted BLE
provisioning, and the HTTPS delivery foundation. It does not change the
telemetry-v3 schemas, archive bytes, or the legacy ingest route.

## Configuration ownership

| Parameter | Type, unit, default | Bounds/validation | Owner and apply policy |
| --- | --- | --- | --- |
| `telemetry.retry_min_ms` | `uint32_t`, ms, `1000` | positive and `<= retry_max_ms` | `AppConfig`; draft/validate/preview/commit, active after reboot |
| `telemetry.retry_max_ms` | `uint32_t`, ms, `60000` | `>= retry_min_ms` | `AppConfig`; draft/validate/preview/commit, active after reboot |
| request timeout | `uint32_t`, ms, `10000` | positive | `NetworkPolicy`; build-time composition |
| Retry-After ceiling | `uint32_t`, ms, `60000` | positive | `NetworkPolicy`; build-time composition |
| retry jitter ceiling | `uint32_t`, ms, `250` | bounded | `NetworkPolicy`; instance policy |
| scan cooldown | `uint32_t`, ms, `15000` | positive | `ProvisioningPolicy`; build-time policy |
| Wi-Fi association timeout | `uint32_t`, ms, `12000` | positive | `ProvisioningPolicy`; build-time policy |
| BLE session timeout | `uint32_t`, ms, `120000` | positive | `ProvisioningPolicy`; build-time policy |
| scan result limit | `uint8_t`, entries, `16` | `1..16` | `ProvisioningPolicy`; bounded RAM |
| BLE frame limit | `uint16_t`, bytes, `192` | `1..192` | `ProvisioningPolicy`; bounded parser |
| archive endpoint | bounded HTTPS string, no default | valid URL and CA required | target composition; compile-time only |
| CA certificate | bounded certificate, no insecure fallback | must be present for target HTTPS | target composition; compile-time only |
| Wi-Fi SSID/password | bounded secret strings | SSID `1..32`, password `0..64` bytes | `SecretStore`; candidate is committed only after association and IP |
| device token | bounded secret string | maximum 128 characters | `SecretStore`; never logged or serialized |
| BLE service enabled | `bool`, `true` | cannot be false for Task 7 target | `ProvisioningPolicy`; build-time |

The BLE service UUIDs are stable and are shared by firmware and web client:

* service: `7d3f0001-7d3f-4a53-9a4b-534947415000`
* command: `7d3f0002-7d3f-4a53-9a4b-534947415000`
* response: `7d3f0003-7d3f-4a53-9a4b-534947415000`

Commands are logical payloads (`GET_STATUS`, `SCAN_WIFI`, `LIST_KNOWN_WIFI`,
`CONNECT_WIFI <ssid> <password>`, and `RETRY_WIFI`) carried in versioned
`F1|request_id|chunk_index|chunk_count|payload` frames. Responses use the
matching `R1` framing and request ID. Every wire frame is capped at 20 bytes
for the default ATT payload; logical commands are capped at 192 bytes and
responses at 512 bytes. Chunks must arrive once and in order. Scan results
contain at most 16 entries and each SSID is capped at 32 bytes. Input is
rejected before it reaches the Wi-Fi manager. GATT characteristics require an
encrypted, authenticated NimBLE-Arduino 2.5.1 link. A per-device NVS setup
secret derives the six-digit passkey displayed by the local commissioning
boundary; the secret is never transmitted by the service or browser. No
custom cryptographic protocol is used.

## Apply and failure policy

Wi-Fi scanning is on demand and rate limited. A candidate credential remains
in RAM until the station reports association and an IP address. Failed
candidates leave the previous saved credential intact. BLE stays advertised
while Wi-Fi connects, retries, or is already connected. Disconnect cancels
the session and clears candidate credentials.

`NetworkWorker` keeps exact durable batch bytes pending through Wi-Fi loss,
timeouts, 429, 5xx, malformed ACKs, and conflicts. A 401/403 enters the
persistent `AuthRequired` state; changing the token is the explicit recovery
operation. No polling call silently re-enables unauthorized delivery.

The target composition object is allocated from the verified PSRAM pool so
the internal DRAM budget remains bounded. PSRAM availability is a board
acceptance requirement and is reported by the existing memory probe.

Calibration, FIS, safety, capacity, GPIO/bus policy, and credentials remain
separate from this network configuration. There is no mutable global
configuration. Runtime connection state, scan results, retry counters, and
BLE sessions are instance-owned RAM.

## Web Bluetooth boundary

The device detail page opens the Wi-Fi dialog without an account login. The
Web Bluetooth picker is started only from the user click on **Connect device**
and is available only in a secure browser context that implements Web
Bluetooth. Passwords are written directly to the BLE characteristic; they are
not sent to an API, URL, analytics system, or browser storage. Unsupported
browsers receive an explicit explanation. Physical BLE/Wi-Fi commissioning
and sensor validation remain HIL work and are not claimed by native tests.
