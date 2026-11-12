# Debug v1 wire contract

This document describes the implemented codecs/DTOs, not full-system acceptance. `registry.json`, C++ serializers/parsers, `lib/debug/codec.ts`, and `internal/devicedebug/contract.go` are the implementation references. HTTP routes/workers and physical transports remain separate implementation/acceptance gates; fixture traffic is TEST-only.

## Shared types and exact bounds

- Levels are `ERROR=0`, `WARN=1`, `INFO=2`, `DEBUG=3` on BLE; JSON carries uppercase names.
- `boot_id` is the existing 16-byte acquisition BootId rendered as exactly 32 lowercase hexadecimal characters. If unavailable, no synthetic ID is valid. Debug event sequence is independent, starts at 1, and never wraps.
- JSON uint64 values are strings in canonical unsigned decimal notation (`0` or nonzero without leading zero), range 0..18446744073709551615. Event `sequence` is positive and strictly increasing within an upload. Monotonic microseconds are not UTC.
- BLE event `request_id` is optional (`-` on BLE, `null` in JSON); when present it is uint32 0..4294967295. `duration_us` is uint32; `arg0` and `arg1` are signed int32. `related_sequence` is optional and when present is uint64. Missing and numeric zero are distinct.
- Module/code pairs and the exact `arg0`/`arg1` meanings are allowlisted below and in `registry.json`; they are not pointers, raw sensor measurements, arbitrary text, credentials, payloads, or free-form errors.

## BLE terminal wire format

Commands use the existing LF-terminated terminal convention before F1 framing. The final LF is removed by the command parser. Tokens in angle brackets are placeholders, not literal text:

```text
GET_DEBUG META
GET_DEBUG PAGE <32-lowercase-hex-boot-id> <canonical-u64-after>
SET_DEBUG_LEVEL ERROR|WARN|INFO|DEBUG
```

Level selection is volatile and the `DBG1|L|n` response reports the effective level. META has no event; PAGE returns at most one event. A missing page event is `-`; META has no synthetic boot fallback. Wire responses are printable ASCII and at most 512 bytes.

```text
DBG1|M|boot|build_hex|level|oldest|latest|overwritten|delivery_lost|usb_lost|captured_us|exhausted
DBG1|P|boot|level|oldest|next_after|overwritten|gap|boot_changed|event-or-dash
DBG1|L|level
```

`M` has exactly 12 pipe-separated fields including `DBG1` and `M`; `P` has exactly 10; `L` exactly 3. Numeric levels are one digit 0..3. Flags (`exhausted`, `gap`, `boot_changed`) are exactly `0` or `1`. Every uint64 is canonical decimal and within uint64. META `build_hex` is lowercase hex for 1..96 printable ASCII bytes (2..192 hex digits); it is not UTF-8 or arbitrary text. PAGE's event tuple, when present, has exactly ten comma-separated fields, in this order:

1. positive uint64 sequence;
2. uint64 monotonic microseconds;
3. level 0..3;
4. module 1..7;
5. registered event code;
6. uint32 request ID or `-`;
7. uint32 duration microseconds;
8. signed int32 arg0;
9. signed int32 arg1;
10. uint64 related sequence or `-`.

An event sequence cannot exceed `next_after`. Empty PAGE and boot-change/gap pages carry `-`, not an invented event. The whole response is strictly decoded; unknown pairs, extra/missing fields, noncanonical numbers, invalid flags, non-ASCII bytes, and oversized responses fail rather than being partially accepted.

### Registry (module/code and argument meanings)

| Module | Code: event | `arg0`; `arg1` |
| --- | --- | --- |
| Runtime (1) | 1: RuntimeBoot | phase: 0 begin, 1 ready, 2 failed; stage: 0 unknown, 1 boot, 2 configuration, 3 acquisition, 4 connectivity, 5 services |
| Runtime (1) | 2: RuntimeStage | stage: 0 begin, 1 ready, 2 failed; stage: 0 unknown, 1 boot, 2 configuration, 3 acquisition, 4 connectivity, 5 services |
| Runtime (1) | 3: RuntimeLoopHealth | state: 0 healthy, 1 late, 2 stalled; elapsed_us (microseconds; 0..2147483647, saturate; zero=no elapsed time) |
| BLE (2) | 10: BleConnected | state: 0 connected, 1 disconnected; reason: 0 normal, 1 timeout, 2 error, 3 unknown |
| BLE (2) | 11: BleDisconnected | reason: 0 normal, 1 timeout, 2 error, 3 unknown; pending_request_count (session-owned admitted async logical requests awaiting backend completion; 0..2147483647, saturate; zero=none) |
| BLE (2) | 12: BleRequestStarted | command_id: 0 unknown, 1 known; request_bytes (bytes; 0..2147483647, saturate; zero=empty) |
| BLE (2) | 13: BleRequestProgress | chunks_sent (SDK notify-call admitted response chunks; 0..2147483647, saturate; zero=none admitted); chunks_total (chunks; 0..2147483647, saturate; zero=no chunks) |
| BLE (2) | 14: BleRequestCompleted | result: 0 success, 1 pending; response_bytes (bytes; 0..2147483647, saturate; zero=empty) |
| BLE (2) | 15: BleRequestRejected | reason: 0 device, 1 decode, 2 transport; error: 0 unknown, 1 timeout, 2 unavailable, 3 protocol, 4 storage, 5 internal |
| BLE (2) | 16: BleIdleExpired | elapsed_us (microseconds; 0..2147483647, saturate; zero=no elapsed time); pending_request_count (session-owned admitted async logical requests awaiting backend completion; 0..2147483647, saturate; zero=none) |
| Wi-Fi (3) | 20: WifiScanStarted | source: 0 operator, 1 internal; attempt (attempts; 0..2147483647, saturate; zero=first attempt not yet counted) |
| Wi-Fi (3) | 21: WifiScanPending | state: 0 pending; elapsed_us (microseconds; 0..2147483647, saturate; zero=no elapsed time) |
| Wi-Fi (3) | 22: WifiScanReady | network_count (networks; 0..2147483647, saturate; zero=none found); duration_us (microseconds; 0..2147483647, saturate; zero=no scan duration) |
| Wi-Fi (3) | 23: WifiScanRateLimited | retry_after_s (seconds; 0..2147483647, saturate; zero=immediate retry); reason: 0 quota |
| Wi-Fi (3) | 24: WifiScanFailed | reason: 0 driver, 1 timeout, 2 unavailable; error: 0 unknown, 1 timeout, 2 unavailable, 3 protocol, 4 storage, 5 internal |
| Wi-Fi (3) | 25: WifiLinkTransition | state: 0 down, 1 connecting, 2 up; reason: 0 normal, 1 retry, 2 error |
| Storage (4) | 30: StorageState | state: 0 unavailable, 1 ready, 2 degraded; error: 0 unknown, 1 timeout, 2 unavailable, 3 protocol, 4 storage, 5 internal |
| Storage (4) | 31: StorageQueue | depth (records; 0..2147483647, saturate; zero=empty); capacity (records; 0..2147483647, saturate; zero=no capacity) |
| Storage (4) | 32: StorageOutcome | result: 0 stored, 1 acked, 2 reclaimed, 3 failed; record_count (records; 0..2147483647, saturate; zero=none in outcome) |
| Acquisition (5) | 40: AcquisitionQuality | quality: 0 ready, 1 warming, 2 invalid, 3 stale; sensor: 0 unknown, 1 gas, 2 weight, 3 distance, 4 temperature_humidity |
| Acquisition (5) | 41: AcquisitionCounters | counter_kind: 0 produced, 1 dropped; count_delta (samples; 0..2147483647, saturate; zero=no change) |
| Acquisition (5) | 42: ProcessingAvailability | state: 0 available, 1 partial, 2 unavailable; reason: 0 unknown, 1 missing_input, 2 stale_input, 3 calibrating, 4 uncalibrated, 5 invalid_input, 6 out_of_domain, 7 invalid_model, 8 trace_limit |
| Calibration (6) | 50: CalibrationOutcome | result: 0 accepted, 1 rejected, 2 cancelled; reason: 0 unknown, 1 invalid_criteria, 2 sensor_disconnected, 3 commit_rejected, 4 timeout, 5 internal |
| Calibration (6) | 51: CalibrationOwner | state: 0 free, 1 USB, 2 BLE, 3 HTTPS; remaining_ms (milliseconds; 0..2147483647, saturate; zero=none remaining) |
| Calibration (6) | 52: ProfileState | state: 0 active, 1 draft, 2 invalid; profile: 0 unknown, 10 calibration, 11 adc, 12 fis, 13 safety, 14 heater, 15 capacity |
| Calibration (6) | 53: ModelState | state: 0 active, 1 unavailable, 2 invalid; model_version (version number; 0..2147483647, saturate; zero=unassigned) |
| HTTPS (7) | 60: HttpsReadiness | state: 0 ready, 1 not_ready; reason: 0 Wi-Fi, 1 time, 2 CA, 3 credential |
| HTTPS (7) | 61: HttpsOutcome | result: 0 durable, 1 transient, 2 rejected; http_class: 0 unknown, 1 2xx, 2 3xx, 3 4xx, 4 5xx |
| HTTPS (7) | 62: HttpsQueue | count (events; 0..2147483647, saturate; zero=empty); oldest_age_us (microseconds; 0..2147483647, saturate; zero=no queued event age) |

Count/measurement arguments are nonnegative signed-int32 projections: adapters clamp values above 2147483647 to that maximum; zero meanings and units are explicit above. BLE link reason 3 is the additive unknown bucket; existing normal/timeout/error values remain 0/1/2. The BLE pending count describes session-owned admitted async backend logical requests still pending (currently scan ownership 0 or 1, snapshotted before disconnect/expiry cleanup), not callback chunks, a rendered R1 response, or an SDK queue; repeated SCAN retains one session-owned request. `chunks_sent` counts response chunks whose SDK `notify` call returned true (call admission only, not radio transmission or client receipt); it does not count a response merely rendered. These BLE clarifications do not change the wire-v1 int32 field shape or add event-specific semantic validation. The debug-owned enum labels are stable contract values, not native enum ordinals: stage is mapped from coarse lifecycle stages; sensor maps native SensorId values to gas/weight/distance/temperature_humidity (never a sensor instance or reading); processing reason is coarsened from fuzzy::FisReason; calibration reason from calibration failure/validation outcomes; profile from acquisition::ProfileKind; error from bounded subsystem error categories; HTTP class records only the response class, never a raw status code. Value 0 is unknown/unclassified wherever offered. Future emitters must explicitly adapt native values into these allowlists; unsupported values map to 0. These meanings do not add event-specific semantic validators: C++/TS/Go boundary checks enforce argument widths and registered module/code pairing, not each event's enum domain. No raw native ordinal, sensor measurement, pointer, identity, or free-form error is part of this registry.

## F1/R1 framing

The existing envelope remains `F1|request_id|chunk_index|chunk_count|payload` for requests and `R1|...` for responses. IDs are decimal tokens 1..10 digits, numeric range 0..4294967295; leading zeroes remain accepted and the original token is preserved byte-for-byte. The empty-token and 11-digit forms are invalid. This matches firmware `parse_uint(field, 0xFFFFFFFFU, request_id)`, which accepts zero and leading zeroes. No ID normalization, recycling, or rollover policy is added here.

Packets are at most 20 bytes, logical bodies at most 512 bytes, and a message uses 1..128 chunks with decimal chunk index 0..count-1. Chunk geometry depends on the full header length, so valid ID/body ranges are not a promise every 512-byte body fits every request ID. A geometrically impossible split/header fails; bytes are never truncated. UTF-8 payload bytes are opaque and can straddle packet boundaries.

Source geometry calculations: the maximum grammar META is 362 ASCII bytes (`DBG1|M|boot(32)|build_hex(192)|level(1)|six uint64(20 each)|flag(1)`, including separators). Maximum grammar PAGE with an event is 223 bytes (32-char boot, three 20-digit page counters, flags, and 114-byte maximum event tuple). Conservative per-component maxima use actual grammar and int32 extrema; they are below the 512-byte logical response bound. Producer fixture bodies in `fixtures/ble.txt` are 53..362 bytes. With IDs `9` and `10`, all seven actual C++ producer fixtures frame and reassemble under both F1/R1. The uint32-max ID correctly frames a one-byte body, but even META cannot fit: its 10-digit ID leaves too little room once the multi-chunk index/count fields grow. This geometry failure is not an ID-range rejection. Geometry is verified in the TS tests; fixtures are not physical traffic.

## HTTPS upload and ACK JSON

Upload schema version 1 has exactly these keys: `schema_version`, `device_id`, `boot_id`, `build_id`, `level`, `captured_monotonic_us`, `oldest_sequence`, `overwritten`, `delivery_lost`, `events`. `device_id` is 1..64 UTF-8 bytes and must exactly match the authorized device identity. `build_id` is 1..96 printable ASCII bytes. Levels are uppercase names. uint64 values, including event sequences/times and envelope counters, are canonical decimal strings; module/event_code, request_id, duration_us, and args are JSON integers with their declared widths; optional request_id and related_sequence are `null` when absent, distinct from zero. Events have exactly `sequence`, `monotonic_us`, `level`, `module`, `event_code`, `request_id`, `duration_us`, `arg0`, `arg1`, `related_sequence`.

The complete serialized request is at most 4096 bytes; it contains 1..8 events and each serialized event is at most 512 bytes. Empty rings do not upload. Reject the entire batch for invalid UTF-8/Unicode, duplicate, unknown, missing, or extra keys, trailing JSON, nesting depth >8, malformed/noncanonical/out-of-range values, unregistered code pairs, non-increasing/zero sequence, event time after captured time, or envelope/event boot mismatch. A request digest is over the exact frozen bytes sent, not a reserialized object.

ACK schema version 1 has exactly `schema_version`, `status`, `device_id`, `boot_id`, `first_sequence`, `last_sequence`, `stored_count`, `deduplicated_count`, `request_sha256`; status is `DURABLE`, identity and boot match the upload, sequence bounds are positive canonical uint64 decimal strings, counts are integers 0..8, and digest is exactly 64 lowercase hex characters. The C++ JSON parser validates ACK shape and values; `matches_upload_ack` separately checks the ACK against expected device ID, the sent snapshot's boot/bounds/counts, and caller-computed SHA-256 of exact request bytes. Parsing alone is not proof that it matches a request. At transport level, HTTP 200 plus successful authorization/TLS and a parsed, context-matching ACK is required; an ACK-shaped body alone, HTTP 202, or failed authorization/TLS is not upload success. ACK semantics do not alter telemetry/archive ACK rules.

## Backend history DTO

The Go `HistoryPage` DTO has `device_id`, `events`, `next_after`, `oldest_available`, `retention_gap`, and `has_more`. Each history event embeds event fields (including the source event's `level`) and adds `ingestion_id`, `received_at`, `boot_id`, `build_id`, `snapshot_level`, `captured_monotonic_us`, `oldest_sequence`, `overwritten`, and `delivery_lost`. `snapshot_level` is the upload's captured effective level; it is distinct from each event's own `level`. `received_at` is server receipt/ingestion time, not MCU event time. History pagination cursors (`ingestion_id`, `next_after`, `oldest_available`) are canonical positive decimal strings when nonzero, bounded by SQLite signed int64; `0` is the initial cursor. MCU sequence and monotonic fields remain uint64 decimal strings and are not narrowed to SQLite cursor range. `retention_gap` reports retention truncation, not proof from a global row-ID gap alone.

## Fixture byte convention

`fixtures/http.json` is a storage fixture with exactly one final LF delimiter: file size 407 bytes, transmitted JSON body size 406 bytes. Remove only that one fixture-storage delimiter to obtain the HTTP payload; do not apply `TrimSpace` or parse/re-marshal before hashing. The digest is computed over exact transmitted bytes. Fixture files are TEST-only; they are never physical/live device output.
