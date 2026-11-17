#!/usr/bin/env python3
"""Strict MQG2/MQG1 fixture checker for complete producer bytes."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator


UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
RATIO_MIN = 0.0893
RATIO_MAX = 0.6559
MIN_CLEAN_AIR_PREHEAT_US = 48 * 60 * 60 * 1_000_000
CURVE_ID = 0x4E48
CURVE_VERSION = 2
CURVE_SHA256 = "sha256:4aa984238efa7d253b77ac828babd40e3a8f10ef14a3a86f8b9f1da0e6dd414b"


class FixtureValidationError(ValueError):
    """A complete raw fixture failed parsing, schema, or semantic checks."""


def _reject_constant(token: str) -> None:
    raise FixtureValidationError(f"syntax: non-finite JSON constant {token}")


def _finite_float(token: str) -> float:
    value = float(token)
    if not math.isfinite(value):
        raise FixtureValidationError(f"syntax: non-finite JSON number {token}")
    return value


def _finite_int(token: str) -> int:
    value = int(token)
    try:
        finite = math.isfinite(float(value))
    except OverflowError:
        finite = False
    if not finite:
        raise FixtureValidationError(f"syntax: non-finite JSON integer {token}")
    return value


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise FixtureValidationError(f"syntax: duplicate key {key!r}")
        result[key] = value
    return result


def strict_json_loads(raw: bytes) -> Any:
    """Parse the full UTF-8 byte stream; reject duplicates and non-finite values."""
    try:
        text = raw.decode("utf-8", errors="strict")
        document = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_constant,
            parse_float=_finite_float,
            parse_int=_finite_int,
        )
    except FixtureValidationError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FixtureValidationError(f"syntax: {exc}") from exc
    if not isinstance(document, dict):
        raise FixtureValidationError("syntax: fixture root must be a JSON object")
    return document


def load_schemas(
    mqg2_schema_path: Path | str,
    mqg1_schema_path: Path | str,
) -> tuple[Draft202012Validator, Draft202012Validator]:
    validators: list[Draft202012Validator] = []
    for path in (mqg2_schema_path, mqg1_schema_path):
        try:
            schema = json.loads(Path(path).read_text(encoding="utf-8"))
            Draft202012Validator.check_schema(schema)
            validators.append(Draft202012Validator(schema))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise FixtureValidationError(f"schema load {path}: {exc}") from exc
    return validators[0], validators[1]


def _schema_check(document: dict[str, Any], validator: Draft202012Validator) -> None:
    errors = sorted(validator.iter_errors(document), key=lambda e: (list(map(str, e.absolute_path)), e.message))
    if errors:
        error = errors[0]
        path = ".".join(str(part) for part in error.absolute_path) or "$"
        raise FixtureValidationError(f"schema: {path}: {error.message}")


def _decimal(value: str, label: str, minimum: int, maximum: int) -> int:
    number = int(value)
    if not minimum <= number <= maximum:
        raise FixtureValidationError(f"semantic: {label} is outside its integer bounds")
    return number


def _pair(left: Any, right: Any, label: str) -> None:
    if (left is None) != (right is None):
        raise FixtureValidationError(f"semantic: {label} version/hash pair must be jointly present or null")


def _is_close(actual: float, expected: float) -> bool:
    # Equality tolerance only covers serialization precision; domain gates below are exact.
    return math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-9)


def _validate_legacy(document: dict[str, Any]) -> None:
    for version_key, hash_key, label in (
        ("profile_version", "profile_hash", "active profile"),
        ("previous_profile_version", "previous_profile_hash", "previous profile"),
        ("adc_profile_version", "adc_profile_hash", "ADC profile"),
    ):
        _pair(document[version_key], document[hash_key], label)
    sequence = document["source_sequence"]
    boot_id = document["source_boot_id"]
    source_mono = document["source_monotonic_us"]
    identity_fields = (sequence, boot_id, source_mono)
    if any(value is None for value in identity_fields) != all(value is None for value in identity_fields):
        raise FixtureValidationError("semantic: legacy source identity fields must be jointly present or null")
    response = _decimal(document["response_monotonic_us"], "legacy response_monotonic_us", 0, UINT64_MAX)
    if source_mono is None:
        if any(document[key] is not None for key in ("age_us", "max_age_us", "fresh")):
            raise FixtureValidationError("semantic: legacy absent source requires null age/fresh fields")
        return
    _decimal(sequence, "legacy source_sequence", 1, UINT64_MAX)
    sample_time = _decimal(source_mono, "legacy source_monotonic_us", 0, UINT64_MAX)
    if response < sample_time:
        raise FixtureValidationError("semantic: legacy response time precedes source time")
    age = document["age_us"]
    maximum_age = document["max_age_us"]
    fresh = document["fresh"]
    age_value = _decimal(age, "legacy age_us", 0, UINT64_MAX) if age is not None else None
    maximum_value = _decimal(maximum_age, "legacy max_age_us", 0, UINT64_MAX) if maximum_age is not None else None
    if age_value is not None and age_value != response - sample_time:
        raise FixtureValidationError("semantic: legacy source age does not match its timestamps")
    if age_value is not None and maximum_value is not None:
        if type(fresh) is not bool or fresh != (age_value <= maximum_value):
            raise FixtureValidationError("semantic: legacy age/freshness mismatch")
    elif fresh is not None:
        raise FixtureValidationError("semantic: legacy uncomparable source must have null freshness")


def _validate_mqg2(document: dict[str, Any]) -> None:
    adc = document["adc"]
    electrical = document["electrical"]
    response = document["response"]
    nh3 = document["nh3"]
    profiles = document["profiles"]
    source = document["source"]
    reference = document["reference"]

    if adc["available"]:
        if (
            adc["method"] != "EFUSE_TP_FIT"
            or not adc["same_sample"]
            or adc["failure"] != "NONE"
            or adc["resolution_bits"] != 12
            or adc["attenuation_db"] != 11
            or adc["adc_unit"] != 1
            or adc["adc_channel"] != 4
            or adc["raw_counts"] is None
            or not 0 < adc["raw_counts"] < 4095
            or adc["pin_mv"] is None
            or adc["pin_mv"] <= 0
        ):
            raise FixtureValidationError("semantic: available ADC metadata requires exact same-sample EFUSE_TP_FIT evidence")
    else:
        if adc["failure"] == "NONE" or adc["pin_mv"] is not None or adc["ao_mv"] is not None:
            raise FixtureValidationError("semantic: unavailable ADC must carry a failure and null calibrated voltages")
        if adc["method"] == "UNAVAILABLE" and any(
            adc[key] is not None for key in ("resolution_bits", "attenuation_db", "adc_unit", "adc_channel")
        ):
            raise FixtureValidationError("semantic: UNAVAILABLE ADC method cannot claim calibration metadata")

    circuit_fields = (
        electrical["circuit_mv"],
        electrical["divider_top_ohm"],
        electrical["divider_bottom_ohm"],
        electrical["topology"],
    )
    if electrical["hardware_confirmed"] and (
        any(value is None for value in circuit_fields) or electrical["hardware_identity"] is None
    ):
        raise FixtureValidationError("semantic: confirmed hardware requires complete electrical identity")
    if adc["ao_mv"] is not None and (
        not adc["available"]
        or adc["pin_mv"] is None
        or any(value is None for value in circuit_fields)
    ):
        raise FixtureValidationError("semantic: AO requires trusted pin voltage and a complete explicit circuit")

    _pair(profiles["version"], profiles["hash"], "active profile")
    _pair(profiles["previous_version"], profiles["previous_hash"], "previous profile")
    _pair(profiles["adc_version"], profiles["adc_hash"], "ADC profile")
    for key in ("version", "previous_version", "adc_version"):
        if profiles[key] is not None:
            _decimal(profiles[key], f"profiles.{key}", 1, UINT32_MAX)
    if profiles["state"] == "MISSING" and profiles["version"] is not None:
        raise FixtureValidationError("semantic: MISSING profile cannot have an active version/hash")
    if profiles["state"] in ("TEST_ONLY", "VALID") and profiles["version"] is None:
        raise FixtureValidationError("semantic: present profile state requires an active version/hash")

    sequence = source["sequence"]
    boot_id = source["boot_id"]
    source_mono = source["monotonic_us"]
    source_identity = (sequence, boot_id, source_mono)
    partial_boot_identity = (
        boot_id is None
        and sequence is not None
        and source_mono is not None
        and document["reason"] in ("SOURCE_IDENTITY_INVALID", "SOURCE_CLOCK_UNKNOWN")
        and source["quality"] is not None
    )
    if (
        any(value is None for value in source_identity) != all(value is None for value in source_identity)
        and not partial_boot_identity
    ):
        raise FixtureValidationError("semantic: source sequence, boot id, and monotonic time are paired")
    response_time = _decimal(source["response_monotonic_us"], "source.response_monotonic_us", 0, UINT64_MAX)
    if source_mono is None:
        if any(source[key] is not None for key in ("age_us", "max_age_us", "fresh")):
            raise FixtureValidationError("semantic: absent source identity requires null age/fresh fields")
    else:
        if sequence is None:
            raise FixtureValidationError("semantic: source sequence is required when monotonic time is present")
        _decimal(sequence, "source.sequence", 1, UINT64_MAX)
        sample_time = _decimal(source_mono, "source.monotonic_us", 0, UINT64_MAX)
        clock_unavailable = (
            document["reason"] in ("SOURCE_IDENTITY_INVALID", "SOURCE_CLOCK_UNKNOWN")
            and all(source[key] is None for key in ("age_us", "max_age_us", "fresh"))
        )
        if response_time < sample_time and not clock_unavailable:
            raise FixtureValidationError("semantic: response time precedes source time")
        age = source["age_us"]
        maximum_age = source["max_age_us"]
        age_value = _decimal(age, "source.age_us", 0, UINT64_MAX) if age is not None else None
        maximum_value = _decimal(maximum_age, "source.max_age_us", 0, UINT64_MAX) if maximum_age is not None else None
        if age_value is not None and age_value != response_time - sample_time:
            raise FixtureValidationError("semantic: source age does not match its timestamps")
        fresh = source["fresh"]
        if age_value is not None and maximum_value is not None:
            if type(fresh) is not bool or fresh != (age_value <= maximum_value):
                raise FixtureValidationError("semantic: source age/freshness mismatch")
        elif fresh is not None:
            raise FixtureValidationError("semantic: uncomparable source must have null freshness")

    identity_failure = document["reason"] == "SOURCE_IDENTITY_INVALID"
    clock_unknown = document["reason"] == "SOURCE_CLOCK_UNKNOWN"
    if identity_failure or clock_unknown:
        if (
            document["quality"] != "INVALID"
            or nh3["status"] != "SOURCE_NOT_READY"
            or source["quality"] is None
            or sequence is None
            or source_mono is None
            or any(value is not None for value in (adc["ao_mv"], response["q"], response["ratio"], nh3["ppm"]))
        ):
            reason = "SOURCE_IDENTITY_INVALID" if identity_failure else "SOURCE_CLOCK_UNKNOWN"
            raise FixtureValidationError(
                f"semantic: {reason} requires an INVALID/SOURCE_NOT_READY diagnostic with null AO/q/ratio/ppm"
            )
    if clock_unknown and (source["quality"] != "READY" or source["fresh"] is not None):
        raise FixtureValidationError(
            "semantic: SOURCE_CLOCK_UNKNOWN requires READY source quality and null freshness"
        )

    for key in ("heating_duration_us",):
        if reference[key] is not None:
            _decimal(reference[key], f"reference.{key}", 0, UINT64_MAX)
    if reference["calibration_epoch_ms"] is not None:
        _decimal(reference["calibration_epoch_ms"], "reference.calibration_epoch_ms", INT64_MIN, INT64_MAX)
    if reference["preparation_confirmed"] and reference["preparation_method"] != "OPERATOR_DECLARED":
        raise FixtureValidationError("semantic: confirmed preparation requires OPERATOR_DECLARED method")
    declared_conditions = any(
        reference[key] is not None
        for key in ("environment", "temperature_centi_c", "humidity_centi_pct")
    )
    expected_conditions = "DECLARED" if declared_conditions else "UNKNOWN"
    if reference["conditions_source"] != expected_conditions:
        raise FixtureValidationError("semantic: conditions_source contradicts declared reference fields")
    if reference["calibration_time_quality"] == "SYNCHRONIZED" and reference["calibration_epoch_ms"] is None:
        raise FixtureValidationError("semantic: SYNCHRONIZED calibration time requires an epoch")
    if reference["calibration_time_quality"] == "UNSYNCED" and reference["calibration_epoch_ms"] is not None:
        raise FixtureValidationError("semantic: UNSYNCED calibration time must remain null")

    if nh3["curve_id"] is None or nh3["curve_version"] is None or nh3["source_sha256"] is None:
        curve_identity = (nh3["curve_id"], nh3["curve_version"], nh3["source_sha256"])
        if any(value is not None for value in curve_identity):
            raise FixtureValidationError("semantic: curve identity fields must be jointly present or null")
    elif (nh3["curve_id"], nh3["curve_version"], nh3["source_sha256"]) != (
        CURVE_ID,
        CURVE_VERSION,
        CURVE_SHA256,
    ):
        raise FixtureValidationError("semantic: curve identity does not match Winsen v1.6 Figure 3")

    q = response["q"]
    q0 = response["q0"]
    ratio = response["ratio"]
    if response["baseline_valid"] != (q0 is not None):
        raise FixtureValidationError("semantic: baseline_valid and q0 nullability disagree")
    if response["baseline_valid"] and profiles["state"] not in ("TEST_ONLY", "VALID"):
        raise FixtureValidationError("semantic: valid q0 baseline requires a present normalized profile")
    if response["reference_kind"] == "NONE" and response["baseline_valid"]:
        raise FixtureValidationError("semantic: NONE reference cannot identify a valid baseline")
    if q is not None and (
        not adc["available"]
        or adc["ao_mv"] is None
        or source["fresh"] is not True
        or source["quality"] != "READY"
    ):
        raise FixtureValidationError(
            "semantic: current q requires available ADC, explicit AO, and a fresh ready source; stale current data is forbidden"
        )
    if ratio is not None:
        if q is None or q0 is None or not response["baseline_valid"]:
            raise FixtureValidationError("semantic: ratio requires current q and a valid q0 baseline")
        if not _is_close(ratio, q / q0):
            raise FixtureValidationError("semantic: ratio coherence requires ratio = q / q0")
    elif q is not None and response["baseline_valid"] and profiles["state"] in ("TEST_ONLY", "VALID"):
        # A missing ratio can still be honest when profile/ADC bindings fail.
        if document["reason"] not in ("ADC_PROFILE_CHANGED", "BASELINE_MISSING"):
            raise FixtureValidationError("semantic: current q and a valid baseline require a ratio unless bindings fail")

    ppm = nh3["ppm"]
    status = nh3["status"]
    if status in ("IN_RANGE", "BELOW_RANGE", "ABOVE_RANGE"):
        if document["reason"] != status or document["quality"] != "READY":
            raise FixtureValidationError(f"semantic: {status} requires matching reason and READY sample quality")
    if ppm is not None and status != "IN_RANGE":
        raise FixtureValidationError("semantic: ppm is permitted only for IN_RANGE")
    if status == "IN_RANGE":
        if ratio is None or ratio < RATIO_MIN or ratio > RATIO_MAX:
            raise FixtureValidationError("semantic: IN_RANGE requires strict ratio domain 0.0893..0.6559")
        if ppm is None:
            raise FixtureValidationError("semantic: IN_RANGE estimate must include ppm")
    if status == "BELOW_RANGE":
        if ratio is None or not ratio > RATIO_MAX:
            raise FixtureValidationError("semantic: BELOW_RANGE requires ratio strictly above 0.6559")
        if ppm is not None:
            raise FixtureValidationError("semantic: BELOW_RANGE ppm must be null")
    if status == "ABOVE_RANGE":
        if ratio is None or not ratio < RATIO_MIN:
            raise FixtureValidationError("semantic: ABOVE_RANGE requires ratio strictly below 0.0893")
        if ppm is not None:
            raise FixtureValidationError("semantic: ABOVE_RANGE ppm must be null")

    if ppm is not None:
        heating = _decimal(reference["heating_duration_us"], "reference.heating_duration_us", 0, UINT64_MAX) if reference["heating_duration_us"] is not None else -1
        if not (
            document["quality"] == "READY"
            and document["reason"] == "IN_RANGE"
            and adc["available"]
            and source["fresh"] is True
            and source["quality"] == "READY"
            and q is not None
            and q0 is not None
            and ratio is not None
            and response["baseline_valid"]
            and response["reference_kind"] == "CLEAN_AIR_CONFIRMED"
            and profiles["state"] in ("TEST_ONLY", "VALID")
            and nh3["method"] == "PIECEWISE_LOG_LOG"
            and document["nh3"]["production_eligible"] is False
            and (nh3["curve_id"], nh3["curve_version"], nh3["source_sha256"])
            == (CURVE_ID, CURVE_VERSION, CURVE_SHA256)
            and reference["preparation_confirmed"]
            and reference["preparation_method"] == "OPERATOR_DECLARED"
            and heating > MIN_CLEAN_AIR_PREHEAT_US
            and reference["sample_count"] is not None
            and reference["sample_count"] >= 3
            and reference["variation_percent"] is not None
        ):
            raise FixtureValidationError("semantic: ppm gate requires all profile, ADC, freshness, clean-air, preheat, and window gates")

    if status == "SOURCE_STALE":
        if source["fresh"] is not False or q is not None or ratio is not None:
            raise FixtureValidationError("semantic: SOURCE_STALE requires false freshness and null current q/ratio")
    if status == "SOURCE_NOT_READY":
        if not identity_failure and (
            (source["quality"] == "READY" and source["fresh"] is not None)
            or q is not None
            or ratio is not None
        ):
            raise FixtureValidationError("semantic: SOURCE_NOT_READY requires non-ready quality/clock and null current q/ratio")
    if status == "ADC_CALIBRATION_UNAVAILABLE" and adc["available"]:
        raise FixtureValidationError("semantic: ADC_CALIBRATION_UNAVAILABLE contradicts available ADC")
    if status == "REFERENCE_NOT_CLEAN_AIR" and (
        response["reference_kind"] == "CLEAN_AIR_CONFIRMED" or ratio is None or ppm is not None
    ):
        raise FixtureValidationError("semantic: REFERENCE_NOT_CLEAN_AIR requires a valid ratio without ppm")
    if status == "PREHEAT_UNCONFIRMED":
        heating = _decimal(reference["heating_duration_us"], "reference.heating_duration_us", 0, UINT64_MAX) if reference["heating_duration_us"] is not None else -1
        if (
            response["reference_kind"] != "CLEAN_AIR_CONFIRMED"
            or ratio is None
            or ppm is not None
            or (
                reference["preparation_confirmed"]
                and reference["preparation_method"] == "OPERATOR_DECLARED"
                and heating > MIN_CLEAN_AIR_PREHEAT_US
            )
        ):
            raise FixtureValidationError("semantic: PREHEAT_UNCONFIRMED must identify a failed strict preheat gate")
    if not identity_failure and source["quality"] in ("WARMING_UP", "HEATER_PHASE", "CRC_ERROR"):
        if document["quality"] != source["quality"] or status != "SOURCE_NOT_READY" or document["reason"] != "SOURCE_NOT_READY":
            raise FixtureValidationError("semantic: root/source quality must preserve warming, heater, or CRC failure honestly")
    if (
        not identity_failure
        and status == "SOURCE_NOT_READY"
        and source["quality"] != "READY"
        and document["quality"] != source["quality"]
    ):
        raise FixtureValidationError("semantic: source-not-ready root quality must preserve the source quality")

    status_mapping = {
        "PROFILE_INVALID": ({"NORMALIZED_PROFILE_INVALID", "ADC_PROFILE_CHANGED"}, {"INVALID"}),
        "UNCALIBRATED": ({"NO_NORMALIZED_PROFILE", "BASELINE_MISSING"}, {"UNCALIBRATED"}),
        "ADC_CALIBRATION_UNAVAILABLE": ({"ADC_CALIBRATION_UNAVAILABLE"}, {"UNAVAILABLE"}),
        "SOURCE_STALE": ({"SOURCE_STALE"}, {"STALE"}),
        "INVALID_RATIO": ({"AO_OUT_OF_RANGE", "INVALID_RATIO"}, {"OUT_OF_RANGE", "INVALID"}),
        "CURVE_UNAVAILABLE": ({"CURVE_UNAVAILABLE"}, {"UNAVAILABLE"}),
        "CURVE_INVALID": ({"CURVE_INVALID"}, {"INVALID"}),
        "REFERENCE_NOT_CLEAN_AIR": ({"REFERENCE_NOT_CLEAN_AIR"}, {"UNCALIBRATED"}),
        "PREHEAT_UNCONFIRMED": ({"PREHEAT_UNCONFIRMED"}, {"UNCALIBRATED"}),
    }
    if status in status_mapping:
        reasons, qualities = status_mapping[status]
        if document["reason"] not in reasons or document["quality"] not in qualities:
            raise FixtureValidationError(f"semantic: {status} contradicts root reason/quality")

    expected_status: str | tuple[str, ...] | None = None
    curve_present = nh3["curve_id"] == CURVE_ID and nh3["curve_version"] == CURVE_VERSION and nh3["source_sha256"] == CURVE_SHA256
    if profiles["state"] == "INVALID" or document["reason"] == "ADC_PROFILE_CHANGED":
        expected_status = "PROFILE_INVALID"
    elif profiles["state"] == "MISSING":
        expected_status = "UNCALIBRATED"
    elif not adc["available"]:
        expected_status = "ADC_CALIBRATION_UNAVAILABLE"
    elif document["reason"] in ("SOURCE_IDENTITY_INVALID", "SOURCE_CLOCK_UNKNOWN"):
        expected_status = "SOURCE_NOT_READY"
    elif source["quality"] != "READY" or source["sequence"] is None or source["fresh"] is None:
        expected_status = "SOURCE_NOT_READY"
    elif source["fresh"] is False:
        expected_status = "SOURCE_STALE"
    elif q is None:
        expected_status = "INVALID_RATIO"
    elif ratio is None:
        expected_status = "UNCALIBRATED"
    elif status in ("CURVE_UNAVAILABLE", "CURVE_INVALID"):
        expected_status = status
    elif not curve_present:
        expected_status = ("CURVE_UNAVAILABLE", "CURVE_INVALID")
    elif response["reference_kind"] != "CLEAN_AIR_CONFIRMED":
        expected_status = "REFERENCE_NOT_CLEAN_AIR"
    elif not (
        reference["preparation_confirmed"]
        and reference["preparation_method"] == "OPERATOR_DECLARED"
        and reference["heating_duration_us"] is not None
        and _decimal(reference["heating_duration_us"], "reference.heating_duration_us", 0, UINT64_MAX)
        > MIN_CLEAN_AIR_PREHEAT_US
    ):
        expected_status = "PREHEAT_UNCONFIRMED"
    elif ratio > RATIO_MAX:
        expected_status = "BELOW_RANGE"
    elif ratio < RATIO_MIN:
        expected_status = "ABOVE_RANGE"
    else:
        expected_status = "IN_RANGE"
    allowed_statuses = (expected_status,) if isinstance(expected_status, str) else expected_status
    if allowed_statuses is not None and status not in allowed_statuses:
        raise FixtureValidationError(
            f"semantic: status precedence requires {', '.join(allowed_statuses)} before the reported {status}"
        )


def validate_json_bytes(
    raw: bytes,
    mqg2_validator: Draft202012Validator,
    mqg1_validator: Draft202012Validator,
) -> str:
    document = strict_json_loads(raw)
    schema_version = document.get("schema")
    if schema_version == "MQG2":
        _schema_check(document, mqg2_validator)
        _validate_mqg2(document)
        return "MQG2"
    if schema_version == "MQG1":
        _schema_check(document, mqg1_validator)
        _validate_legacy(document)
        return "MQG1"
    raise FixtureValidationError(f"schema: unsupported schema discriminator {schema_version!r}")


def _project_root() -> Path:
    return Path(__file__).resolve().parents[4]


def _collect_files(inputs: list[Path]) -> list[Path]:
    files: list[Path] = []
    for path in inputs:
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files.extend(sorted(child for child in path.glob("*.json") if child.is_file()))
        else:
            raise FixtureValidationError(f"input does not exist or is not a file/directory: {path}")
    unique = list(dict.fromkeys(files))
    if not unique:
        raise FixtureValidationError("no fixture files found")
    return unique


def main(argv: list[str] | None = None) -> int:
    root = _project_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="fixture JSON files and/or a directory of JSON fixtures")
    parser.add_argument("--schema", type=Path, default=root / "contracts/calibration/mq135-nh3/v2/schema.json")
    parser.add_argument("--legacy-schema", type=Path, default=root / "contracts/calibration/mq135-relative/v1/schema.json")
    args = parser.parse_args(argv)
    try:
        mqg2, mqg1 = load_schemas(args.schema, args.legacy_schema)
        files = _collect_files(args.inputs)
    except FixtureValidationError as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        return 2

    passed = 0
    failed = 0
    for path in files:
        try:
            raw = path.read_bytes()
            version = validate_json_bytes(raw, mqg2, mqg1)
            digest = hashlib.sha256(raw).hexdigest()
            print(f"PASS {path}: parsed + schema + semantic ({version}); bytes={len(raw)} sha256={digest}")
            passed += 1
        except (OSError, FixtureValidationError) as exc:
            print(f"FAIL {path}: {exc}", file=sys.stderr)
            failed += 1
    print(f"SUMMARY files={len(files)} passed={passed} failed={failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
