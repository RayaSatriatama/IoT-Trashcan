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

No network, storage, telemetry, or calibration maintenance workflow is present in
this slice. `profile_store.h` is only the typed boundary for the later atomic
profile store. Profile persistence, active-pointer commit, rollback, and
maintenance commands are deferred to Task 4.
