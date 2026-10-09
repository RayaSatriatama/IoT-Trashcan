# Sensor commissioning guide

This guide covers the native USB maintenance workflow for first-time sensor
setup. It is software-ready documentation; it is not evidence of physical
sensor calibration. Every reference value must come from the instrument and
wiring actually used on the board.

## Safety and ownership

- Use native USB locally. Do not send raw readings or passwords as command
  values.
- A failed capture, validation, preview, or commit leaves the active profile
  unchanged.
- `UNCALIBRATED` records may be used only when the target adapter reports the
  sensor-specific raw, identity, CRC, completion, and heater evidence needed
  by that family.
- MQ-7 heater timing is disabled until a validated heater profile exists.
- TEST_ONLY fixtures stay in native tests and must never be activated as a
  production profile.

## Common command sequence

Enter maintenance mode, select one target, provide explicit criteria, then
capture the anchors required by that target:

```text
maintenance enter
calibration start <target>
calibration criteria samples <positive-count>
calibration criteria max_span <positive-value>
calibration capture <anchor>
calibration status
calibration validate
calibration preview
calibration commit
profile status <target>
```

Use `max_span_counts` for HX711 and MQ ADC targets. Use temperature and
humidity span fields for SHT31. The target must remain selected for the whole
session. `calibration cancel` is safe before criteria are complete and removes
the session overlay without changing the active profile.

## Target-specific setup

### HX711 organic, inorganic, and B3

Required physical setup: load cell, HX711 wiring, stable tare condition, and
an explicitly known positive reference mass.

```text
calibration start hx711_organic
calibration criteria samples 10
calibration criteria max_span_counts 1000
calibration criteria reference_mass_mg <measured-mass-mg>
calibration capture tare
calibration capture reference
calibration validate
calibration preview
calibration commit
```

The resulting profile stores tare and signed counts-per-kilogram. Zero,
negative, non-finite, or unstable scaling is rejected.

### Ultrasonic organic, inorganic, and B3

Required physical setup: documented transducer model, safe ECHO level, known
empty/full distances, and the validated echo conversion. The engineering
parameters are explicit session criteria; no hidden distance conversion is
used when a profile is absent.

```text
calibration start ultrasonic_organic
calibration criteria echo_mm_per_us_milli <validated-value>
calibration criteria echo_timeout_us <positive-us>
calibration criteria inter_sensor_gap_us <positive-us>
calibration criteria overshoot_tolerance_mm_milli <nonnegative-mm-milli>
calibration criteria samples 10
calibration criteria max_span <validated-mm-span>
calibration capture empty
calibration capture full
calibration validate
calibration preview
calibration commit
```

The profile requires `D_empty > D_full`, finite conversion parameters, and
positive timing values. The raw echo duration remains evidence; it is not
silently reported as millimetres.

### MQ-4, MQ-135, MQ-7, and MQ-8

Required physical setup: the installed module's datasheet, a validated ADC
reference/divider, and an approved heater profile for MQ-7. Firmware stores
ADC evidence only. It does not invent ppm, R0, OGI, or alarm thresholds.

```text
calibration start mq4
calibration criteria reference_mv <validated-mv>
calibration criteria full_scale_counts <adc-full-scale>
calibration criteria divider_numerator <divider-numerator>
calibration criteria divider_denominator <divider-denominator>
calibration criteria samples 10
calibration criteria max_span_counts <validated-count-span>
calibration capture reference
calibration validate
calibration preview
calibration commit
```

MQ-7 additionally requires a validated heater profile and a suitable heater
phase. A floating ADC or an unsafe heater state is diagnostic evidence only.

### DS18B20 B3

Required physical setup: one-wire pull-up, the installed device, and a known
reference temperature. With no profile, an explicit calibration session
enables bounded ROM discovery; normal acquisition does not touch the bus when
the profile is missing.

```text
calibration start ds18b20_b3
calibration criteria resolution_bits <9-12>
calibration criteria samples 5
calibration criteria max_span <centi-c-span>
calibration criteria reference_temperature_centi_c <measured-centi-c>
calibration capture reference
calibration validate
calibration preview
calibration commit
```

ROM family/CRC, scratchpad CRC, conversion completion, and resolution are
checked before the trial is accepted. The committed profile contains the
discovered ROM and the measured offset.

### SHT31 addresses 0x44 and 0x45

Required physical setup: the addressed breakout, shared I2C wiring, and
independent temperature and humidity references.

```text
calibration start sht31_44
calibration criteria samples 5
calibration criteria max_temperature_span_centi_c <span>
calibration criteria max_humidity_span_centi_pct <span>
calibration criteria reference_temperature_centi_c <measured-centi-c>
calibration criteria reference_humidity_centi_pct <measured-centi-percent>
calibration capture reference
calibration validate
calibration preview
calibration commit
```

Repeat with `sht31_45` for the second address. Temperature and humidity CRCs
are validated independently; one address failing does not disable the other.

## Diagnostics and recovery

Use `sensor status <target>` for a bounded, read-only snapshot of one target.
Targets are `hx711_organic`, `hx711_inorganic`, `hx711_b3`,
`ultrasonic_organic`, `ultrasonic_inorganic`, `ultrasonic_b3`, `mq4`,
`mq135`, `mq7`, `mq8`, `ds18b20_b3`, `sht31_44`, and `sht31_45`.

The response has only bounded fields: `s` sensor, `st` stream, `av` evidence
availability, `q` quality, `r` reason, `raw` raw presence, `age` monotonic
age in microseconds, `fr` freshness, `f` family-relevant protocol flags, `p`
profile presence, `pv` profile version, `c` calibration state, and `a`/`x`
accepted/rejected evidence counts. It never prints raw values, ROM/scratchpad,
profile hashes, Wi-Fi credentials, or device tokens. `fr=STALE` means the
latest observation is marked stale or exceeds the fixed diagnostic window of
5 seconds for HX711, ultrasonic, DS18B20, and SHT31, or 10 seconds for
MQ-4, MQ-135, and MQ-8. MQ-7 uses the active valid heater cycle
(`high_clean_us + low_detect_us`); without that profile, `fr=UNAVAILABLE`.

Use `calibration status`, `profile status <target>`, and the normal `status`
for their respective lifecycle and device summaries.
`calibration cancel` clears the target-local overlay. A timeout, transport
loss, reboot, or failed commit also discards the draft. To retry, start a new
session and provide all criteria again. Use the existing profile export and
rollback commands before replacing a known-good profile.

No physical sensor, heater, actuator, calibration reference, or power-loss
result is claimed until the owner runs the documented setup and captures the
corresponding evidence.
