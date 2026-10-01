# Fixture corpus

The valid corpus is organized by semantic case and the invalid corpus by stable error
taxonomy. The sixteen valid JSON batch files are loaded by the Go corpus test; invalid
cases retain their expected stable category in `fixture-manifest.json` and are also
covered by procedural boundary tests where generating a 32 KiB or 16 KiB payload would
add noise to the fixture. Required categories include
UTC-null, calibrated HX711, ultrasonic profile, raw MQ, MQ-7 heater phase, DS18B20,
SHT31 temperature+humidity, multi-source result, unavailable fuzzy/safety, clock anchor,
activation metadata, mixed batch, max uint64, Unicode metadata, near-limit result,
duplicate keys, unsupported schema, invalid uint64, count/hash/identity conflicts,
body/record/depth/string limits, unknown enums, malformed profiles, and strict fields.
