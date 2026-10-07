# SIGAP acquisition foundation

This library contains bounded, hardware-independent acquisition seams for Task 2.
Each driver emits a typed sample attempt through `RecordFactory`; raw evidence is
retained when a conversion profile is unavailable or a read fails. The scheduler
uses monotonic time and never waits on a sensor.

Sampling cadence is owned by `AppConfig`; physical acquisition behavior is owned
by typed hardware profiles. Ultrasonic timing profiles may override the named
engineering defaults (30 ms echo timeout and 60 ms shared acoustic gap). Missing
or invalid timing profiles use those defaults only for raw acquisition and do not
produce hardware-validated profile provenance. Echo-to-distance conversion is
profile-owned. Engineering defaults are not hardware validation.

| Value | Owner |
| --- | --- |
| Sample period | `AppConfig` |
| Ultrasonic echo timeout and acoustic gap | `UltrasonicProfile` plus shared conservative resolver |
| `D_empty`, `D_full`, tolerance, echo conversion | `UltrasonicProfile` |
| HX711 tare and scale | `Hx711Profile` |
| MQ ADC reference and divider | `MqAdcProfile` |
| MQ-7 cycle timing | `Mq7HeaterProfile` |
| DS18B20 ROM, resolution, offset | `Ds18b20Profile` |
| SHT31 address and offsets | `Sht31Profile` |
| GPIO | `BoardConfig` |

DS18B20 conversion wait is derived from the profile resolution (9--12 bit). The
target adapter reads the scratchpad configuration, applies a mismatched profile
resolution while preserving alarm bytes, and verifies the readback before
sampling. It does not issue `COPY SCRATCHPAD`, so profile application does not
add EEPROM writes on every boot. CRC and one-wire slot timings remain protocol
invariants. MQ drivers expose
ADC data and optional engineering millivolts only; ppm, R0, OGI, and alarm
thresholds are deliberately absent. MQ-7 phase timings are supplied by
`Mq7HeaterProfile` and are not universal production constants.

Profile identity is a typed `(kind, target)` key. MQ-7 ADC and heater profiles
therefore coexist without layout ambiguity. Profile persistence is implemented
as a bounded deterministic envelope with an inactive-slot/readback/hash check and
pointer-last activation. `MemoryProfileStore`
provides native fault injection; `NvsProfileStore` uses the separate
`sigap_profile` namespace on target. `ProfileManager` keeps immutable encoded
snapshots, stages imports through explicit validate/preview/commit commands, supports rollback and
reset, and emits a non-durable activation audit event. Secrets remain in the
separate `sigap_secret` namespace and are never included in profile exports.
Calibration-built profiles also carry bounded provenance in the hashed envelope:
boot identity, monotonic capture time, UTC/time-quality state, accepted
reference values, units, and an evidence summary. Legacy envelopes without this
optional payload remain readable but are not treated as new calibration evidence.

The maintenance layer uses native USB CDC on target with a fixed line buffer and
per-poll byte budget. It exposes the AppConfig draft lifecycle, redacted secret
status/write/clear operations, evidence-backed target calibration sessions, and
bounded profile export/import/validate/preview/commit/rollback/reset routing.
Import uses a declared size and SHA-256 digest before staging; finish never
activates a draft. Reboot is deferred until the response
has been written. A bounded audit ring records config/profile activation events;
overflow is observable. Persistent logger, telemetry, and network workers remain
out of scope. Profile persistence maintenance commands remain intentionally
separate from Task 4's future long-lived service orchestration.

Graph startup rejects only global composition failures such as missing
mandatory reader dependencies, invalid active runtime configuration, or
scheduler capacity failure. Sensor-local states such as a missing or invalid
calibration profile, profile-apply failure, unplugged sensor, CRC error, or
heater phase produce typed channel records and do not stop unrelated channels.
In particular, the DS18B20 channel reports `Uncalibrated` with
`ProfileMissing` when its profile is absent and never requests a one-wire
conversion without a configured ROM.
