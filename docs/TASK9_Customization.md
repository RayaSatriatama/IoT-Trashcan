# Task 9 customization and ownership

Task 9 adds integration policy only. It does not create a second configuration
system or move calibration/model values into runtime globals.

| Value | Type/unit | Owner | Bounds/default | Apply policy |
| --- | --- | --- | --- | --- |
| WiFi retry/backoff | typed duration | existing WiFi policy/AppConfig where already defined | validated positive duration; existing defaults | snapshot at boot or explicit committed config, per existing owner |
| BLE session timeout | typed duration | BLE provisioning policy | positive bounded duration | compile-time policy unless an existing versioned config field owns it |
| BLE frame/payload limit | `size_t` bytes | BLE protocol policy | fixed bounded protocol limit | compile-time; reject oversized input |
| HTTPS response deadline | typed duration | network transport policy | positive bounded deadline | validated policy, no blocking acquisition path |
| archive batch/body limits | count/bytes | frozen telemetry-v3 contract | 128 records, 32 KiB body | immutable contract |
| storage worker queue | bounded count | storage runtime policy | measured target capacity | instance-owned; no mutable global |
| storage slice budget | bytes/time | storage runtime policy | measured and documented | per invocation; must account for nested I/O |
| SD transport pins/mode | board policy | `BoardConfig`/build | SD_MMC 1-bit GPIO39/38/40 on confirmed board | build-time only; no silent SPI substitution |
| telemetry credentials | secret bytes | `SecretStore`/device-auth boundary | never exposed to frontend/logs | candidate test then atomic persistence |
| BLE prototype access | unauthenticated standard GATT; no passkey, pairing requirement, or authenticated characteristic property | identity is routing data only; Wi-Fi candidate credentials persist through the application SecretStore only after association and IP success; profile mutation remains session-owned and validated | compile-time BLE policy; physical interaction remains a separate operator gate |
| staging TLS | file paths and boolean requirement | Go process-local server configuration | certificate and key must be paired regular files; required mode fails closed | process startup only; loopback staging, separate SQLite database |
| calibration/FIS/safety/capacity | typed versioned profiles | profile manager | profile validators and identity/hash | explicit validate/preview/commit; no synthetic production profile |
| first-time ultrasonic engineering criteria | session-owned typed values: `echo_mm_per_us_milli`, `echo_timeout_us`, `inter_sensor_gap_us`, `overshoot_tolerance_mm_milli` | native maintenance session, then validated `UltrasonicProfile` | positive conversion/timing; nonnegative overshoot; explicit target | discarded on cancel/timeout; persisted only in committed profile |
| first-time MQ ADC criteria | session-owned `uint32_t` values: reference mV, ADC full-scale, divider numerator/denominator | native maintenance session, then validated `MqAdcProfile` | all positive and bounded; MQ-7 also requires a validated heater profile | discarded on failure; no ppm/R0/OGI is produced |
| DS18B20 first-time identity | discovered ROM, resolution bits 9-12, reference temperature | calibration evidence plus `Ds18b20Profile` | ROM family/CRC, scratchpad CRC, completion, identity and resolution match | ROM is persisted only after explicit commit; missing profile stays uncalibrated |
| SHT31 first-time identity | fixed address `0x44` or `0x45`, temperature and humidity references | calibration evidence plus `Sht31Profile` | address binding and independent temperature/humidity CRC | committed per address; other address remains independent |
| connection state, scan results, counters | runtime objects | instance-owned `RuntimeState` | bounded memory | reset on instance lifecycle, not global mutable state |
| debug level | `debug::Level` (`ERROR/WARN/INFO/DEBUG`) | volatile `DebugService` instance | configured build default; runtime changes are not persisted | explicit typed debug command; fresh service resets; never AppConfig/SecretStore |
| debug upload endpoint | `SIGAP_DEBUG_ENDPOINT`, optional HTTPS URL | out-of-tree build configuration; validated as `https://.../api/device-debug/device/{device_code}/events` | absent/empty disables upload; max 192 bytes; no telemetry-endpoint fallback | build definition is consumed at firmware boot; source configuration is not evidence of installed configuration |
| debug upload device code | `SIGAP_DEBUG_DEVICE_CODE`, optional exact backend `devices.device_code` | out-of-tree build configuration; exact byte-for-byte match with endpoint route | absent/empty disables upload; max 64 UTF-8 bytes; never derived from MAC, BLE name/wire ID, control ID, or database row ID | paired with the endpoint at build/boot; never normalized or logged |
| observed loop-entry gap | immutable `uint64_t` microseconds; Runtime debug event code 3 | `runtime_debug_policy` compile-time constants | healthy `<=1,000,000`; late `<=5,000,000`; otherwise stalled | observed at normal app-loop entry, coalesced by class; no runtime toggle or persistence |
| TLS worker stack budget | immutable `uint32_t` bytes; `network::kTlsWorkerStackBytes` | shared network compile-time policy | 16,384 bytes for archive, control, and debug HTTPS workers; conservative budget, not measured safe size | target high-water/heap qualification required; no runtime override |
| reclaim workspace reserve | immutable `uint64_t` bytes; `LittleFsArchiveStore::kMigrationMetadataReserveBytes` | storage compile-time policy | 4,096 bytes above retained-copy bytes; not a universal filesystem overhead bound | no AppConfig/NVS/runtime override; a per-instance capacity probe must succeed before migration-file mutation |

## Ownership rules

- Board GPIO, SD mode, protocol constants, and hard limits are build-time
  policy. They are not operator credentials or scientific calibration.
- AppConfig remains a validated per-boot snapshot. Existing NVS A/B and reboot
  activation semantics remain authoritative.
- SecretStore owns WiFi/device credentials. Passwords, bearer tokens, and CA
  private material must not appear in source, fixtures, browser storage, URLs,
  telemetry, or logs.
- Calibration, capacity, FIS, and Safety are versioned profiles. TEST_ONLY
  fixtures never become active PRODUCTION profiles automatically.
- Queues, buffers, latches, retry counters, and worker state are instance
  members with explicit bounds. No mutable non-const global configuration.

## Apply and failure policy

Network and storage workers must continue acquisition when WiFi or backend
delivery fails. A failed candidate credential preserves the previous working
credential. A malformed or conflicting ACK cannot advance delivery or reclaim
pending bytes. Missing sensor/profile evidence remains unavailable and is not
replaced with synthetic values. Resource or quota failure produces observable
backpressure/gap evidence according to the existing archive policy.

Every new policy value requires a typed validator, a focused native test, and
documentation of its owner and activation boundary before use in target
composition.

The Runtime3 loop-gap policy is an immutable engineering heuristic, not a
measured board deadline or watchdog threshold. It classifies only the last gap
observed between normal `SigapApp::loop()` entries: healthy through 1,000,000 us,
late through 5,000,000 us, and stalled above that. The first entry emits healthy
with elapsed zero; later events emit only on class transitions, with the gap
clamped to `INT32_MAX`. Filtering at the existing DEBUG service level does not
disable observation. A refused event is attempted once for that class, while a
full ring follows the service's normal overwrite behavior. Clock regression
rebases the timestamp without emitting or changing the cached class; zero is a
valid timestamp. Setup resets per-instance history. This retrospective event
cannot detect a loop that never re-enters and proves neither liveness nor
watchdog behavior.

## Reclaim workspace containment

`storage/littlefs_archive_store.h` contains the filesystem archive implementation;
`storage/archive_media.h` retains the compatibility include and other adapters.
Full-copy reclaim requires additional physical workspace while the original
archive remains present. Pending-byte quota is not evidence of physical free
space. Before removing, opening, truncating or writing a migration destination,
the store requires a bound per-instance capacity probe and enough available
bytes for `source_bytes - matched_frame_bytes + 4096`, with checked subtraction
and addition. The fixed reserve is a conservative policy margin, not a measured
LittleFS metadata/COW upper bound or an atomic filesystem-space reservation.

The current application binds its LittleFS store to the installed SDK's
status-returning `esp_littlefs_info("spiffs", &total, &used)`. This label matches
the actual `LittleFS.begin(false)` default and selected partition table. SDK
error, zero total, or used greater than total makes capacity unavailable.
Unknown, failed, overflowing or insufficient capacity rejects reclaim before
migration mutation, preserves source/pending identities and returns blocked or
retryable state through the existing delivery path. Two independent Arduino
`totalBytes()`/`usedBytes()` calls are not used, since either can return zero on
failure. No synthetic capacity or quota-based fallback is present in production.

The shared `SdArchiveStore` also fails closed if no trustworthy capacity probe
is bound; this correction does not provide an SD probe or establish SD reclaim
acceptance. The currently reviewed DNS sensor-SIM candidate does not select the
SD opt-in target. Host fixtures explicitly supply their own synthetic capacity.

This is containment, not a bounded-workspace compaction redesign. A full archive
can remain `LOGGING_BLOCKED`; unacknowledged or unsupported retained frames must
not be erased to force progress. The original read-flags panic and exact custom
SDK allocator provenance remain separately unproven. Host tests and cached
builds do not establish target panic repair, reclaim progress, or end-to-end
durability. The new host matrix is exercised with
`python3 -m unittest tests.test_reclaim_workspace -v`; its 64-bit reported-size
fault injections test arithmetic defensiveness, not reachable ESP32 geometry.

## BLE prototype access decision

The owner-authorized prototype uses ordinary GATT access without a passkey,
forced pairing, MITM, authenticated characteristic permissions, or an
application authentication gate. `GET_DEVICE_ID` is a routing check, not proof
of authentication. BLE profile operations retain their existing calibration session ownership,
bounds, replay/expiry checks, validation, preview, and explicit commit flow.
The existing `CONNECT` operation remains a bounded WiFi candidate flow and
persists credentials through the application SecretStore only after association
and IP success. Direct AppConfig/profile mutation is not exposed as a BLE write.
Because BLE authentication is absent in this prototype, nearby provisioning
attempts remain possible; hardening is deferred by the owner. Application
persistence and USB maintenance permissions remain separate.

The BLE-only setup-secret store and USB handoff are removed. This change does
not read, erase, migrate, rotate, or delete existing NVS records or bonds. It
also does not change the application SecretStore, Wi-Fi/device credentials,
HTTP/API authorization, HTTPS certificate validation, or profile persistence.
Authenticated BLE security acceptance is `DEFERRED_BY_OWNER`, not PASS. Task 9
and physical acceptance remain HOLD until their separate gates have evidence.

The disposable Go staging process accepts `SIGAP_DB_PATH`,
`SIGAP_HTTP_ADDR`, `SIGAP_TLS_CERT_FILE`, `SIGAP_TLS_KEY_FILE`, and
`SIGAP_TLS_REQUIRED`. TLS-required mode rejects incomplete certificate/key
pairs and missing files before opening the database or listener. Credentials,
private keys, and test identities remain outside the repository.

Dependency remediation is documented in `docs/task9-dependency-audit.md` and
uses only lockfile-backed, narrow overrides. It does not create a second
runtime configuration system.

## Web calibration ownership

| Variable | Type and bound | Owner | Persistence/apply policy | Test |
| --- | --- | --- | --- | --- |
| Device-control payload | bounded JSON bytes, max 16 KiB | Go `devicecontrol.Store` | SQLite transaction before queue acknowledgement | `internal/devicecontrol` store tests |
| Command lease | typed duration, default 30 s | Go `devicecontrol.Store` instance | runtime lease; expiry closes the session | claim/expiry tests |
| Command expiry | RFC3339 timestamp supplied by operator client | command row | rejected when expired; no device mutation | expiry tests |
| Allowed operator origin | process configuration | injected `HTTPOptions` | request boundary only; mismatch fails closed | HTTP origin tests |
| Sensor capability units | bounded typed response | device-control handler | static contract metadata, no scientific threshold | capability endpoint test |
| BLE calibration frame | fixed protocol bound from BLE policy | firmware BLE adapter | compile-time framing; oversized input rejected | BLE parser tests |
| Maintenance response frame | fixed 384-byte payload bound | firmware maintenance protocol | compile-time; typed calibration previews reject truncation; debug uses the same bound without protocol expansion | maintenance and debug USB tests |
| Calibration anchor | enum `tare/reference/empty/full` | firmware `CommandRouter` session | explicit per capture, discarded on cancel/timeout | BLE adapter and maintenance tests |
| Browser operator token | in-memory string, max 512 chars | website component state | never persisted; cleared with component/session | API and wizard tests |

The remote queue is a command boundary, not a calibration engine. Firmware
evidence, profile construction, validation, preview, and commit remain the
authoritative operations. The production composition must inject a real
operator authorizer and wire a nonblocking firmware HTTPS polling adapter
before remote calibration is enabled. Nil authorizers fail closed.

Debug upload is independently optional. Define both `SIGAP_DEBUG_ENDPOINT`
and `SIGAP_DEBUG_DEVICE_CODE` in an out-of-tree build configuration to enable
it. The complete endpoint must use HTTPS and the existing device-debug upload
route, with a single path segment that decodes to the exact configured code
(UTF-8, at most 64 bytes; percent-encoded reserved bytes are supported).
The endpoint is at most 192 bytes; query/fragment, userinfo, wrong root,
malformed percent escapes and mismatched identity are unavailable. Partial,
oversized, or mismatched settings make the typed policy unavailable; no live
deployment values are required for TEST fixtures. The worker still obtains its
token only from the existing SecretStore boundary and requires the existing
trusted CA and valid SNTP time. Do not add token macros, persist these settings
in AppConfig/NVS, or log secrets. Updating source settings does not establish
that a particular installed device has those settings; they apply when built
and booted.
