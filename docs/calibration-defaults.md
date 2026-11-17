# Calibration defaults and persistence boundaries

## Profiles supplied by the firmware

Factory profile construction is deliberately narrow. HX711 organic, organic ultrasonic, MQ135 ADC, and SHT31 at address `0x44` are the four reference-only acquisition targets. Full-graph bootstrap enables thirteen targets, including additional HX711, ultrasonic, ADC, DS18B20, and SHT31 targets. Heater configuration and operational capacity are not calibration defaults; MQ4 is not part of the reference-only target set.

Factory SHT31 profiles use the corresponding bus address and zero additional temperature and humidity offsets. They carry `FACTORY_DEFAULT` provenance and are valid profiles, but describe interface defaults—not verified sensor accuracy. Other profiles that need field calibration are persisted as invalid templates with explicit unresolved calibration state. A template is not an active calibration and must not be used to claim calibrated measurements.

## Storage and precedence

Active profiles live in the A/B slot and activation-metadata namespace (`active`, `previous`, generation, and hashes). Unresolved defaults live separately in the `d_*` namespace. Creating or preserving a template does not activate it or alter the active pointer.

Bootstrap only initializes an eligible pristine SHT factory profile. For other targets it creates a template only when no default exists. Existing saved profiles, including newer calibrations, take precedence and are not overwritten. Subsequent real calibration is committed through the normal profile persistence flow and remains the selected active profile.

Malformed or corrupt stored defaults are preserved as evidence and block replacement; storage read errors also block bootstrap writes. There are no tombstones or automatic repair/rewrite behavior. Recovery or replacement of corrupt data requires a separately reviewed operation, not bootstrap.

## Evidence limits

Native regression sources and synthetic `MemoryProfileStore` behavior establish source-level intent only. Source review, native builds, target builds, generated artifacts, and device observations are separate evidence classes; one does not imply another. **No device persistence proof is available here.** No physical calibration, tare/mass/geometry value, or ppm accuracy is asserted by these defaults.
