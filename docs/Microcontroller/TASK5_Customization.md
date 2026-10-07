# SIGAP Task 5 customization guide

Task 5 keeps policy, physical calibration, scientific model profiles, board
identity, credentials, and runtime state in separate ownership boundaries.
Changing a value in source does not overwrite an active NVS snapshot.

## Parameter ownership

| Parameter | Type and unit | Default or allowed range | Owner | Apply policy |
| --- | --- | --- | --- | --- |
| `sampling.hx711_period_ms` | `uint32_t`, ms | factory default `100`; validated positive range | `AppConfig` | draft, validate, preview, commit; active after reboot |
| `sampling.mq_period_ms` | `uint32_t`, ms | factory default `100`; validated positive range | `AppConfig` | draft, validate, preview, commit; active after reboot |
| `sampling.ultrasonic_period_ms` | `uint32_t`, ms | factory default `1000`; validated positive range | `AppConfig` | draft, validate, preview, commit; active after reboot |
| `sampling.ds18b20_period_ms` | `uint32_t`, ms | factory default `1000`; validated positive range | `AppConfig` | draft, validate, preview, commit; active after reboot |
| `sampling.sht31_period_ms` | `uint32_t`, ms | factory default `2000`; validated positive range | `AppConfig` | draft, validate, preview, commit; active after reboot |
| `telemetry.summary_period_ms` | `uint32_t`, ms | factory default `15000`; validated range | `AppConfig` | draft, validate, preview, commit; active after reboot |
| `telemetry.max_records_per_batch` | `uint16_t`, records | factory default `128`; hard bounded | `AppConfig` | draft, validate, preview, commit; active after reboot |
| `telemetry.retry_min_ms/max_ms` | `uint32_t`, ms | `1000` / `60000`; min <= max | `AppConfig` | draft, validate, preview, commit; active after reboot |
| pending storage quotas | `uint64_t`, bytes | factory defaults in `config_defaults.h`; reserve enforced | `AppConfig` | draft, validate, preview, commit; active after reboot |
| HX711 tare and scale | typed counts and `float` counts/kg | absent until a valid profile exists | calibration profile | maintenance workflow; new acquisition cycle/profile boundary |
| ultrasonic empty/full/tolerance and timing | typed mm/us fields | absent until validated profile; explicit engineering fallback only for raw acquisition | calibration profile | maintenance workflow; next acquisition attempt |
| MQ ADC reference/divider | typed counts/mV ratio | absent until validated profile | calibration profile | maintenance workflow; next acquisition attempt |
| MQ-7 heater cycle | typed microsecond durations and phase policy | absent until approved profile | heater profile | safe cycle boundary |
| DS18B20 ROM/resolution/offset | typed ROM, bits, centi-degrees | absent until validated profile | calibration profile | maintenance workflow; next conversion |
| SHT31 address/offsets | typed address and centi-units | absent until validated profile | calibration profile | maintenance workflow; next reading |
| FIS membership/rules/grid | versioned bounded model profile | `UNCONFIGURED` unless explicitly TEST_ONLY fixture | FIS model profile | validate, preview, commit; new processing cycle |
| B3 safety thresholds/hysteresis | versioned bounded model profile | unavailable until approved scientific profile | Safety model profile | validate, preview, commit; latch policy remains explicit |
| GPIO, bus, flash/PSRAM, protocol constants | compile-time typed constants | board/build contract | BoardConfig/BuildConfig | firmware build only |
| Wi-Fi/token credentials | secret blobs | no source default | SecretStore | provisioning boundary only |
| counters, queues, sensor health, latches | runtime-owned value state | reset/boot scoped | module instance | RAM only; never serialized as AppConfig |

The only factory AppConfig definition is
`firmware/include/sigap/config/config_defaults.h`. Validation lives in
`firmware/lib/config/src/config/`; model/profile validation lives beside each
typed model. A draft is not visible to runtime consumers. No production model
threshold or sensor reading is supplied by Task 5.

## Model maintenance workflow

Model envelopes use independent typed keys (`fis_organic`, `fis_inorganic`,
`fis_b3`, and `safety_b3`) and the bounded commands:

```text
model import begin <key> <size> <sha256>
model import chunk [<offset>] <hex>
model import finish <key>
model validate <key>
model preview <key>
model commit <key>
model cancel <key>
model rollback <key>
```

Import finish only stages a draft. The active model identity remains unchanged
until explicit validate, preview, and commit succeed. The target adapter uses
two bounded slots and pointer-last activation metadata; failed readback,
integrity, or activation leaves the previous committed generation authoritative.
Rollback is per typed key. A TEST_ONLY model is accepted only in explicit
native fixtures and is not installed by target composition as PRODUCTION.

At a processing boundary, the runtime copies the selected FIS model and the
B3 safety model into an immutable cycle snapshot. A profile update therefore
cannot alter an in-flight result. Safety latch state is instance-owned and is
not recreated for each cycle.

## TEST_ONLY model fixtures

Native tests may create explicitly marked TEST_ONLY FIS and Safety profiles to
verify mathematics and state transitions. These fixtures are not installed by
target composition and are not evidence of hardware calibration or production
safety approval.
