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
| USB BLE passkey handoff | six decimal digits, ephemeral output | existing NVS-derived BLE setup secret plus `LocalPasskeyHandoff` | one disclosure after explicit `maintenance enter`; unavailable/repeated requests fail | instance-owned commissioning state; underlying secret is never rotated |
| staging TLS | file paths and boolean requirement | Go process-local server configuration | certificate and key must be paired regular files; required mode fails closed | process startup only; loopback staging, separate SQLite database |
| calibration/FIS/safety/capacity | typed versioned profiles | profile manager | profile validators and identity/hash | explicit validate/preview/commit; no synthetic production profile |
| first-time ultrasonic engineering criteria | session-owned typed values: `echo_mm_per_us_milli`, `echo_timeout_us`, `inter_sensor_gap_us`, `overshoot_tolerance_mm_milli` | native maintenance session, then validated `UltrasonicProfile` | positive conversion/timing; nonnegative overshoot; explicit target | discarded on cancel/timeout; persisted only in committed profile |
| first-time MQ ADC criteria | session-owned `uint32_t` values: reference mV, ADC full-scale, divider numerator/denominator | native maintenance session, then validated `MqAdcProfile` | all positive and bounded; MQ-7 also requires a validated heater profile | discarded on failure; no ppm/R0/OGI is produced |
| DS18B20 first-time identity | discovered ROM, resolution bits 9-12, reference temperature | calibration evidence plus `Ds18b20Profile` | ROM family/CRC, scratchpad CRC, completion, identity and resolution match | ROM is persisted only after explicit commit; missing profile stays uncalibrated |
| SHT31 first-time identity | fixed address `0x44` or `0x45`, temperature and humidity references | calibration evidence plus `Sht31Profile` | address binding and independent temperature/humidity CRC | committed per address; other address remains independent |
| connection state, scan results, counters | runtime objects | instance-owned `RuntimeState` | bounded memory | reset on instance lifecycle, not global mutable state |

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

## Commissioning-only additions

The USB maintenance parser accepts `ble passkey` only after an explicit
`maintenance enter` command. The router emits the six-digit passkey once per
service instance, then returns `BLE_PASSKEY_ALREADY_DISCLOSED` for repeats.
`status`, startup diagnostics, telemetry, HTTP responses, browser storage, and
repository files never contain the passkey or the underlying NVS setup secret.
The handoff is a local commissioning aid for a display-less board; it does not
weaken BLE authenticated MITM protection.

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
| Calibration anchor | enum `tare/reference/empty/full` | firmware `CommandRouter` session | explicit per capture, discarded on cancel/timeout | BLE adapter and maintenance tests |
| Browser operator token | in-memory string, max 512 chars | website component state | never persisted; cleared with component/session | API and wizard tests |

The remote queue is a command boundary, not a calibration engine. Firmware
evidence, profile construction, validation, preview, and commit remain the
authoritative operations. The production composition must inject a real
operator authorizer and wire a nonblocking firmware HTTPS polling adapter
before remote calibration is enabled. Nil authorizers fail closed.
