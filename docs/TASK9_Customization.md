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
| calibration/FIS/safety/capacity | typed versioned profiles | profile manager | profile validators and identity/hash | explicit validate/preview/commit; no synthetic production profile |
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
