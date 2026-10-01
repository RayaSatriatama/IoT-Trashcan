# SIGAP archive telemetry v3 contract

This directory freezes the additive archive contract for `POST /api/telemetry/batches`.
The existing `POST /api/ingest` summary-v2 route remains unchanged. The v3 contract
does not invoke legacy normalization, fuzzy inference, safety evaluation, or simulator
inference.

## Wire invariants

- Request body: at most 32 KiB; total records in all four arrays: at most 128.
- One serialized record: at most 16 KiB; JSON nesting: at most 12; free-form UTF-8 strings: at most 512 bytes.
- Object keys are unique, UTF-8 is valid, numbers are finite, and unknown fields/enums are rejected.
- Every record has `record_id`, `record_hash`, `kind`, `device_id`, `boot_id`, decimal-string `sequence`, decimal-string `monotonic_us`, nullable `recorded_at`, `time_quality`, `config_ref`, and `profile_refs`.
- `record_id` is exactly `<device_id>/<boot_id>/<sequence>`.
- `record_hash` is `sha256:` plus 64 lowercase hexadecimal characters over the RFC 8785 JCS canonical complete record with `record_hash` omitted.
- Every wire uint64 uses a canonical decimal string. `sequence_sort` is a derived 20-digit zero-padded key and is not part of the request.
- JSON numbers use JCS/ECMAScript normalization. Non-finite values and lexical `-0` are rejected; finite decimal/exponent values are canonicalized by JCS rather than stringized or rounded by application code. `null` remains distinct from an absent field.
- `body_hash` is SHA-256 over exact HTTP request bytes and is stored outside the request body.
- `manifest_ids` is intentionally absent. Profile manifests are separate provenance objects and may be persisted later.

## Frozen typed payloads

Sample identity is bound to the logical stream. The frozen map is:

| Sensor ID | Stream |
| --- | --- |
| `hx711_organic`, `ultrasonic_organic`, `mq4`, `mq135`, `sht31_44` | `organic` |
| `hx711_inorganic`, `ultrasonic_inorganic` | `inorganic` |
| `hx711_b3`, `ultrasonic_b3`, `mq7`, `mq8`, `ds18b20_b3`, `sht31_45` | `b3` |

Sample payloads are sensor-family specific and reject unknown nested fields:

- HX711: `raw.counts`; `converted.kg` or `null`.
- Ultrasonic: `raw.echo_us`; `converted.distance_mm` and `fill_percent`, each nullable when unavailable.
- MQ-4/MQ-135/MQ-8: `raw.adc_counts`; MQ-7 additionally carries `raw.heater_phase`; conversion is `adc_mv` or `null`.
- DS18B20: ROM, scratchpad, CRC/completion evidence, resolution, and nullable `temperature_c`.
- SHT31: temperature/humidity ticks with independent CRC evidence and nullable temperature/humidity values.

No ppm, R0, OGI, or generic gas threshold is part of the sample contract.

Results use fixed `processed`, `fuzzy`, and `safety` objects. Model traces, when present,
contain only the typed model reference, memberships, rules, and aggregate fields. Events
and metadata use typed payload variants for each declared event/metadata type; arbitrary
payload objects are rejected.

The body and record limits bound total nested memory. There is no hidden 128-item cap on
source IDs, traces, or nested arrays; semantic limits apply where the contract requires a
record count or source-ID count bound.

## Profile and configuration provenance

`config_ref` identifies the immutable active AppConfig snapshot. `profile_refs` identify
non-secret calibration, capacity, heater, ADC, FIS, and safety profiles. A missing
manifest does not invalidate a structurally valid raw record.

## Retry and conflict boundary

After the first send attempt, a batch keeps the same `batch_id`, exact body bytes, and
body hash. Same `(device_id,batch_id,body_hash)` is an idempotent replay. The same batch
identity with another body hash, or a record identity with another record hash, is a
whole-batch conflict and maps to HTTP 409. No partial commit is implied by this contract.

Engineering defaults, scientific model values, credentials, HTTP handlers, migrations,
archive persistence, and profile maintenance are outside this Task 3 freeze.
