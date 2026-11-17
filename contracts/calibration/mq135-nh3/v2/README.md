# MQ135 normalized NH3 result, MQG2

**Contract and fixtures are test-only. They are not physical-device output, a calibrated NH3 concentration, or a safety classification. Physical HIL and scientific/ppm-accuracy claims remain HOLD.**

## Shape and units

`schema.json` is the closed Draft 2020-12 shape. Every root group and every documented field is required; nested groups reject extra keys. Missing measurements are explicit `null`, never omitted, substituted with zero, `NaN`, or infinity. The strict checker additionally rejects duplicate keys, invalid UTF-8, non-finite numeric tokens/overflow, and inconsistent cross-field states.

- `adc`: raw ADC counts; calibrated pin and analog-output voltages in mV; method `EFUSE_TP_FIT` or `UNAVAILABLE`; same-sample/failure evidence and hardware metadata. `available` is independent of source freshness. When true, metadata must be 12-bit, 11 dB, ADC unit 1/channel 4, same-sample, successful, and raw counts must be strictly between the 12-bit rails. When false, calibrated pin/AO are null and a failure code is present. Raw counts may remain when genuinely observed. AO requires trusted pin voltage and an explicit circuit.
- `electrical`: circuit mV, divider resistances in ohms, topology `1` or `2`, hardware confirmation, and optional bounded identity. Values supplied but not physically confirmed are not promoted to confirmed measurements.
- `response`: dimensionless `q`, `q0`, and `ratio`; reference kind `NONE`, `OPERATIONAL`, or `CLEAN_AIR_CONFIRMED`; and baseline validity. A valid ratio requires current `q` and a valid `q0`; `q0` may remain present while the current sample is stale.
- `nh3`: `ppm`, status, curve identity, method `PIECEWISE_LOG_LOG`, and `production_eligible: false`. Non-null ppm is allowed only for `IN_RANGE` after all profile, ADC, source-freshness, baseline, clean-air, preheat, and sample-window gates pass. All other statuses carry null ppm.
- `profiles`: state `MISSING`, `INVALID`, `TEST_ONLY`, or `VALID`; active, previous, and ADC version/hash pairs. Version references are canonical decimal strings bounded to uint32; hashes are `sha256:` plus 64 lowercase hex digits.
- `source`: 32-hex boot ID, canonical uint64 decimal sequence/times, age and maximum age in microseconds, freshness, and source quality. The response monotonic timestamp is always present. When comparable, `age_us = response_monotonic_us - monotonic_us` and `fresh` is exactly `age_us <= max_age_us`; absent-source fields, including `quality`, are null. A returned source may still have known quality when identity or clock is invalid, so null identity does not imply null quality. Source quality includes `WARMING_UP`, `HEATER_PHASE`, and `CRC_ERROR`; these remain distinct from concentration validity.
- `reference`: optional environment, temperature in centi-degrees C, humidity in centi-percent, declared/unknown conditions, heating duration in microseconds, operator preparation state/method, sample count, variation percent, signed UTC calibration epoch milliseconds, and time quality. Nullable temperature and humidity are independent.

Only 64-bit integral values and profile references use decimal strings; schema-bounded smaller integers and engineering measurements remain JSON numbers. Signed epoch strings are canonical int64. The validator's 1e-6 relative / 1e-9 absolute tolerance applies only to the `ratio = q / q0` serialized-number coherence check. It never affects curve-domain admission.

## Curve and range meanings

The firmware identity is curve ID `0x4E48` (`20040`), version `2`, and digest `sha256:4aa984238efa7d253b77ac828babd40e3a8f10ef14a3a86f8b9f1da0e6dd414b`. It binds the `rs_r0` column of `docs/mq135-winsen-nh3-curve.csv` to the NH3 trace in Zhengzhou Winsen's MQ135 datasheet v1.6, Figure 3. The curve is a digitized research approximation; the figure is not a manufacturer numeric specification. The Olimex material is comparison-only, not this curve's source. The curve identity is present even when an estimate is unavailable; only an absent/invalid identity is represented by all three identity fields being null.

The inclusive estimator domain is **0.0893 <= ratio <= 0.6559**. `ratio > 0.6559` is `BELOW_RANGE` (below the nominal 10 ppm endpoint) and `ratio < 0.0893` is `ABOVE_RANGE` (above the nominal 1000 ppm endpoint); both retain an available ratio and require null ppm. There is no extrapolation, epsilon, clamping, or rounding a ratio into range. Exactly 0.6559 and 0.0893 are in-domain. The displayed research estimate is non-selective and is not a certified concentration or safety threshold.

When several prerequisites fail, the producer's released status precedence is: `PROFILE_INVALID`; `UNCALIBRATED`; `ADC_CALIBRATION_UNAVAILABLE`; `SOURCE_NOT_READY`; `SOURCE_STALE`; `INVALID_RATIO`; `CURVE_UNAVAILABLE`/`CURVE_INVALID`; `REFERENCE_NOT_CLEAN_AIR`/`PREHEAT_UNCONFIRMED`; then `IN_RANGE`/`BELOW_RANGE`/`ABOVE_RANGE`. Root/source quality continues to report sensor conditions (including warming, heater phase, and CRC errors), not NH3 validity. The validator checks this precedence for observable inputs. The parent must compare it with the final producer report before downstream release.

`OPERATIONAL` may carry a valid baseline and ratio, but does not permit ppm. A ppm estimate additionally requires `CLEAN_AIR_CONFIRMED`, `preparation_confirmed: true`, `preparation_method: OPERATOR_DECLARED`, and documented heating strictly greater than 48 hours (172800000000 us). These are still declarations in synthetic tests, not physical proof.

## Test-only data and regeneration

Native producer fixture bytes are actual C++ test output, not physical-device output; in-memory Python mutations are **SYNTHETIC / TEST_ONLY**. Positive status requires producer provenance and passing schema/semantic checks, not a filename. Preserve the historical contradictory `worst-case-valid.json` unchanged as invalid stress evidence; a distinct producer-generated replacement may be positive only when its bytes pass those checks. The validator's directory scan is nonrecursive; pass nested diagnostic files explicitly.

From the repository root, regenerate only through the actual native producer fixture test (this overwrites the canonical fixture files):

```sh
set -e
cd firmware
SIGAP_MQG2_FIXTURE_DIR="$(realpath ../contracts/calibration/mq135-nh3/v2/fixtures)" \
  pio test -e native -f 'native/test_mq135_normalized_producer'
cd ..
```

Preserve the old fixture bytes/hashes before an authorized regeneration. The environment variable makes that producer write the exact test-produced bytes; without it, the native test compares its output byte-for-byte with the canonical fixtures. Do not hand-edit or synthesize fixture JSON and call it producer output.

Run the strict byte, syntax, schema, and semantic checks on the generated directory (or explicit fixture files):

```sh
python3 firmware/test/host/mq135_normalized_fixture_checks/validate_fixtures.py \
  contracts/calibration/mq135-nh3/v2/fixtures
python3 -m unittest discover -s firmware/test/host/mq135_normalized_fixture_checks -v
```

MQG1 documents are routed to the unchanged `contracts/calibration/mq135-relative/v1/schema.json` plus its preserved legacy time/identity semantic checks; this release does not rewrite MQG1. The checker uses the installed Python `jsonschema` Draft 2020-12 validator and adds no package or project dependency.
