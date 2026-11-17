"""Focused trust-boundary tests; the baseline is a TEST_ONLY mutation oracle.

The baseline values are copied from frozen Worker-B in-range.json. It is a
synthetic in-memory clone, not a claim of producer output or a device fixture.
"""
from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from validate_fixtures import FixtureValidationError, load_schemas, validate_json_bytes


PROJECT_ROOT = Path(__file__).resolve().parents[4]
SCHEMA_PATH = PROJECT_ROOT / "contracts/calibration/mq135-nh3/v2/schema.json"
LEGACY_SCHEMA_PATH = PROJECT_ROOT / "contracts/calibration/mq135-relative/v1/schema.json"
MQG2_FIXTURE_DIR = PROJECT_ROOT / "contracts/calibration/mq135-nh3/v2/fixtures"

# SYNTHETIC TEST_ONLY clone of the frozen in-range sample for negative mutations.
TEST_ONLY_BASE = json.loads(r'''{"schema":"MQG2","sensor":"mq135","quality":"READY","reason":"IN_RANGE","adc":{"raw_counts":2308,"available":true,"pin_mv":2308,"ao_mv":3846.66675,"method":"EFUSE_TP_FIT","same_sample":true,"failure":"NONE","resolution_bits":12,"attenuation_db":11,"adc_unit":1,"adc_channel":4},"electrical":{"circuit_mv":5000,"divider_top_ohm":10000,"divider_bottom_ohm":15000,"topology":1,"hardware_confirmed":true,"hardware_identity":"SYNTHETIC"},"response":{"q":0.29982669,"q0":1,"ratio":0.29982669,"reference_kind":"CLEAN_AIR_CONFIRMED","baseline_valid":true},"nh3":{"ppm":76.3925197,"status":"IN_RANGE","curve_id":20040,"curve_version":2,"source_sha256":"sha256:4aa984238efa7d253b77ac828babd40e3a8f10ef14a3a86f8b9f1da0e6dd414b","method":"PIECEWISE_LOG_LOG","production_eligible":false},"profiles":{"state":"TEST_ONLY","version":"1","hash":"sha256:ec0c6fc69a46afdd01a32f07e975c300191804f53c2455f6c1a4cc3ee88a4090","previous_version":null,"previous_hash":null,"adc_version":"1","adc_hash":"sha256:423f1dbb744ef6e158aa9773a2bb03f0d8785080fefff9e487731d0b2b1e0482"},"source":{"boot_id":"11111111111111111111111111111111","sequence":"4","monotonic_us":"4100","response_monotonic_us":"4100","age_us":"0","max_age_us":"1000000","fresh":true,"quality":"READY"},"reference":{"environment":"SYNTHETIC","temperature_centi_c":2537,"humidity_centi_pct":4100,"conditions_source":"DECLARED","heating_duration_us":"180000000000","preparation_confirmed":true,"preparation_method":"OPERATOR_DECLARED","sample_count":3,"variation_percent":0,"calibration_epoch_ms":null,"calibration_time_quality":"UNSYNCED"}}''')


class FixtureValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.mqg2, cls.mqg1 = load_schemas(SCHEMA_PATH, LEGACY_SCHEMA_PATH)

    def check(self, document: dict) -> str:
        raw = json.dumps(document, allow_nan=False, separators=(",", ":")).encode()
        return validate_json_bytes(raw, self.mqg2, self.mqg1)

    def rejected(self, document: dict, message: str) -> None:
        with self.assertRaisesRegex(FixtureValidationError, message):
            self.check(document)

    def test_existing_mqg1_fixture_uses_v1_schema_and_semantics(self) -> None:
        legacy = PROJECT_ROOT / "contracts/calibration/mq135-relative/v1/fixtures/ready-test-only.json"
        self.assertEqual(validate_json_bytes(legacy.read_bytes(), self.mqg2, self.mqg1), "MQG1")

    def test_valid_synthetic_baseline_passes(self) -> None:
        self.assertEqual(self.check(copy.deepcopy(TEST_ONLY_BASE)), "MQG2")

    def test_duplicate_keys_are_rejected_before_schema_validation(self) -> None:
        with self.assertRaisesRegex(FixtureValidationError, "duplicate key"):
            validate_json_bytes(b'{"schema":"MQG2","schema":"MQG2"}', self.mqg2, self.mqg1)

    def test_nonstandard_nan_is_rejected(self) -> None:
        with self.assertRaisesRegex(FixtureValidationError, "non-finite"):
            validate_json_bytes(b'{"schema":"MQG2","x":NaN}', self.mqg2, self.mqg1)

    def test_nonstandard_infinity_is_rejected(self) -> None:
        with self.assertRaisesRegex(FixtureValidationError, "non-finite"):
            validate_json_bytes(b'{"schema":"MQG2","x":Infinity}', self.mqg2, self.mqg1)

    def test_overflowing_json_float_is_rejected(self) -> None:
        with self.assertRaisesRegex(FixtureValidationError, "non-finite"):
            validate_json_bytes(b'{"schema":"MQG2","x":1e9999}', self.mqg2, self.mqg1)

    def test_json_integer_outside_finite_binary64_range_is_rejected(self) -> None:
        raw = b'{"schema":"MQG2","x":' + (b"9" * 400) + b"}"
        with self.assertRaisesRegex(FixtureValidationError, "non-finite"):
            validate_json_bytes(raw, self.mqg2, self.mqg1)

    def test_unknown_root_key_is_rejected(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["extra"] = True
        self.rejected(doc, "Additional properties")

    def test_required_nullable_field_cannot_be_omitted(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        del doc["response"]["q"]
        self.rejected(doc, "'q' is a required property")

    def test_time_must_be_decimal_string_not_json_integer(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["source"]["response_monotonic_us"] = 4100
        self.rejected(doc, "response_monotonic_us")

    def test_uint64_time_is_bounded(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["source"]["response_monotonic_us"] = "18446744073709551616"
        self.rejected(doc, "outside its integer bounds")

    def test_signed_calibration_epoch_is_bounded_to_int64(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["reference"]["calibration_epoch_ms"] = "9223372036854775808"
        self.rejected(doc, "outside its integer bounds")

    def test_profile_reference_is_bounded_to_uint32(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["profiles"]["version"] = "4294967296"
        self.rejected(doc, "outside its integer bounds")

    def test_curve_id_must_match_winsen_identity(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["nh3"]["curve_id"] = 7
        self.rejected(doc, "curve_id")

    def test_curve_digest_must_match_winsen_identity(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["nh3"]["source_sha256"] = "sha256:" + "0" * 64
        self.rejected(doc, "source_sha256")

    def test_estimation_method_is_exact(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["nh3"]["method"] = "LINEAR"
        self.rejected(doc, "method")

    def test_adc_method_is_exact(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["adc"]["method"] = "PROFILE_LINEAR"
        self.rejected(doc, "method")

    def test_available_adc_requires_exact_calibration_metadata(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["adc"]["attenuation_db"] = 10
        self.rejected(doc, "available ADC")

    def test_ppm_cannot_claim_wrong_status_or_ratio_direction(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["nh3"]["status"] = "ABOVE_RANGE"
        doc["reason"] = "ABOVE_RANGE"
        self.rejected(doc, "permitted only for IN_RANGE")

    def test_ppm_requires_clean_air_reference(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["reference_kind"] = "OPERATIONAL"
        self.rejected(doc, "ppm gate")

    def test_preheat_gate_is_strictly_greater_than_48_hours(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["reference"]["heating_duration_us"] = "172800000000"
        self.rejected(doc, "preheat")

    def test_fresh_flag_must_match_age(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["source"]["fresh"] = False
        self.rejected(doc, "freshness")

    def test_adc_unavailable_precedes_source_stale(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["quality"] = "UNAVAILABLE"
        doc["reason"] = "ADC_CALIBRATION_UNAVAILABLE"
        doc["adc"].update({
            "available": False,
            "pin_mv": None,
            "ao_mv": None,
            "method": "UNAVAILABLE",
            "same_sample": False,
            "failure": "EFUSE_UNSUPPORTED",
            "resolution_bits": None,
            "attenuation_db": None,
            "adc_unit": None,
            "adc_channel": None,
        })
        doc["response"]["q"] = None
        doc["response"]["ratio"] = None
        doc["source"]["response_monotonic_us"] = "5004100"
        doc["source"]["age_us"] = "5000000"
        doc["source"]["fresh"] = False
        doc["nh3"]["ppm"] = None
        doc["nh3"]["status"] = "ADC_CALIBRATION_UNAVAILABLE"
        self.assertEqual(self.check(doc), "MQG2")
        doc["nh3"]["status"] = "SOURCE_STALE"
        doc["quality"] = "STALE"
        doc["reason"] = "SOURCE_STALE"
        self.rejected(doc, "status precedence")

    def test_stale_source_cannot_retain_current_q_ratio_or_ppm(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["source"]["response_monotonic_us"] = "5004100"
        doc["source"]["age_us"] = "5000000"
        doc["source"]["fresh"] = False
        doc["quality"] = "STALE"
        doc["reason"] = "SOURCE_STALE"
        doc["nh3"]["ppm"] = None
        doc["nh3"]["status"] = "SOURCE_STALE"
        self.rejected(doc, "fresh ready source")

    def test_available_adc_cannot_have_null_pin_voltage(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["adc"]["pin_mv"] = None
        self.rejected(doc, "available ADC")

    def test_q_ratio_and_q0_must_be_numerically_coherent(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["ratio"] = 0.2
        self.rejected(doc, "ratio coherence")

    def test_profile_reference_pairs_are_atomic(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["profiles"]["hash"] = None
        self.rejected(doc, "profile version/hash pair")

    def test_conditions_source_reflects_declared_values(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["reference"]["conditions_source"] = "UNKNOWN"
        self.rejected(doc, "conditions_source")

    def test_inclusive_upper_curve_endpoint_is_in_range(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["q"] = 0.6559
        doc["response"]["ratio"] = 0.6559
        doc["nh3"]["ppm"] = 10.0
        self.assertEqual(self.check(doc), "MQG2")

    def test_inclusive_lower_curve_endpoint_is_in_range(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["q"] = 0.0893
        doc["response"]["ratio"] = 0.0893
        doc["nh3"]["ppm"] = 1000.0
        self.assertEqual(self.check(doc), "MQG2")

    def test_value_just_above_upper_endpoint_is_not_promoted_to_ppm(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["q"] = 0.6559000000000001
        doc["response"]["ratio"] = 0.6559000000000001
        self.rejected(doc, "strict ratio domain")

    def test_value_just_below_lower_endpoint_is_not_promoted_to_ppm(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["q"] = 0.08929999999999999
        doc["response"]["ratio"] = 0.08929999999999999
        self.rejected(doc, "strict ratio domain")

    def test_below_range_status_requires_ratio_strictly_above_maximum(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["q"] = 0.6559
        doc["response"]["ratio"] = 0.6559
        doc["nh3"]["ppm"] = None
        doc["nh3"]["status"] = "BELOW_RANGE"
        doc["reason"] = "BELOW_RANGE"
        self.rejected(doc, "BELOW_RANGE")

    def test_above_range_status_requires_ratio_strictly_below_minimum(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["q"] = 0.0893
        doc["response"]["ratio"] = 0.0893
        doc["nh3"]["ppm"] = None
        doc["nh3"]["status"] = "ABOVE_RANGE"
        doc["reason"] = "ABOVE_RANGE"
        self.rejected(doc, "ABOVE_RANGE")

    def test_nonnull_ppm_is_rejected_when_response_quantity_is_null(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["response"]["q"] = None
        doc["response"]["ratio"] = None
        self.rejected(doc, "strict ratio domain")

    def test_absent_source_quality_may_be_null(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["quality"] = "UNAVAILABLE"
        doc["reason"] = "ADC_CALIBRATION_UNAVAILABLE"
        doc["adc"].update({
            "raw_counts": None,
            "available": False,
            "pin_mv": None,
            "ao_mv": None,
            "method": "UNAVAILABLE",
            "same_sample": False,
            "failure": "NOT_ATTEMPTED",
            "resolution_bits": None,
            "attenuation_db": None,
            "adc_unit": None,
            "adc_channel": None,
        })
        doc["response"]["q"] = None
        doc["response"]["ratio"] = None
        doc["nh3"]["ppm"] = None
        doc["nh3"]["status"] = "ADC_CALIBRATION_UNAVAILABLE"
        doc["source"].update({
            "boot_id": None,
            "sequence": None,
            "monotonic_us": None,
            "age_us": None,
            "max_age_us": None,
            "fresh": None,
            "quality": None,
        })
        self.assertEqual(self.check(doc), "MQG2")

    def test_source_clock_unknown_keeps_freshness_and_current_values_null(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["quality"] = "INVALID"
        doc["reason"] = "SOURCE_CLOCK_UNKNOWN"
        doc["adc"]["ao_mv"] = None
        doc["response"]["q"] = None
        doc["response"]["ratio"] = None
        doc["source"]["age_us"] = None
        doc["source"]["max_age_us"] = None
        doc["source"]["fresh"] = None
        doc["nh3"]["ppm"] = None
        doc["nh3"]["status"] = "SOURCE_NOT_READY"
        self.assertEqual(self.check(doc), "MQG2")

    def test_source_not_ready_quality_is_preserved_without_current_quantities(self) -> None:
        for state in ("WARMING_UP", "HEATER_PHASE", "CRC_ERROR"):
            with self.subTest(state=state):
                doc = copy.deepcopy(TEST_ONLY_BASE)
                doc["quality"] = state
                doc["reason"] = "SOURCE_NOT_READY"
                doc["adc"]["ao_mv"] = None
                doc["response"]["q"] = None
                doc["response"]["ratio"] = None
                doc["nh3"]["ppm"] = None
                doc["nh3"]["status"] = "SOURCE_NOT_READY"
                doc["source"]["quality"] = state
                self.assertEqual(self.check(doc), "MQG2")

    def test_actual_identity_invalid_producer_diagnostic_is_accepted(self) -> None:
        raw = (MQG2_FIXTURE_DIR / "source-identity-invalid.json").read_bytes()
        document = json.loads(raw)
        self.assertEqual(validate_json_bytes(raw, self.mqg2, self.mqg1), "MQG2")
        self.assertEqual((document["quality"], document["reason"]),
                         ("INVALID", "SOURCE_IDENTITY_INVALID"))
        self.assertEqual(document["nh3"]["status"], "SOURCE_NOT_READY")
        self.assertEqual((document["source"]["quality"], document["source"]["fresh"]),
                         ("READY", True))
        self.assertEqual(document["response"]["q0"], 1)
        self.assertTrue(all(value is None for value in (
            document["adc"]["ao_mv"], document["response"]["q"],
            document["response"]["ratio"], document["nh3"]["ppm"])))

    def test_actual_unknown_source_boot_diagnostic_is_accepted(self) -> None:
        raw = (MQG2_FIXTURE_DIR / "source-identity-unavailable.json").read_bytes()
        document = json.loads(raw)
        self.assertEqual(validate_json_bytes(raw, self.mqg2, self.mqg1), "MQG2")
        self.assertEqual((document["quality"], document["reason"]),
                         ("INVALID", "SOURCE_CLOCK_UNKNOWN"))
        self.assertEqual(document["nh3"]["status"], "SOURCE_NOT_READY")
        self.assertEqual(document["source"]["quality"], "READY")
        self.assertIsNone(document["source"]["boot_id"])
        self.assertIsNotNone(document["source"]["sequence"])
        self.assertIsNotNone(document["source"]["monotonic_us"])
        self.assertIsNone(document["source"]["fresh"])
        self.assertEqual(document["response"]["q0"], 1)
        self.assertTrue(all(value is None for value in (
            document["adc"]["ao_mv"], document["response"]["q"],
            document["response"]["ratio"], document["nh3"]["ppm"])))

    def test_identity_failure_precedes_nonready_source_quality(self) -> None:
        document = json.loads((MQG2_FIXTURE_DIR / "source-identity-invalid.json").read_bytes())
        document["source"]["quality"] = "CRC_ERROR"
        self.assertEqual(self.check(document), "MQG2")

    def test_identity_invalid_reason_cannot_be_promoted_to_root_ready(self) -> None:
        document = json.loads((MQG2_FIXTURE_DIR / "source-identity-invalid.json").read_bytes())
        document["quality"] = "READY"
        self.rejected(document, "SOURCE_IDENTITY_INVALID")

    def test_identity_invalid_reason_cannot_carry_derived_quantities(self) -> None:
        document = json.loads((MQG2_FIXTURE_DIR / "source-identity-invalid.json").read_bytes())
        document["adc"]["ao_mv"] = 3846.66675
        document["response"]["q"] = 0.29982669
        document["response"]["ratio"] = 0.29982669
        document["nh3"]["ppm"] = 76.3925197
        self.rejected(document, "SOURCE_IDENTITY_INVALID")

    def test_source_clock_unknown_reason_requires_unknown_freshness(self) -> None:
        document = json.loads((MQG2_FIXTURE_DIR / "source-identity-invalid.json").read_bytes())
        document["reason"] = "SOURCE_CLOCK_UNKNOWN"
        self.rejected(document, "SOURCE_CLOCK_UNKNOWN")

    def test_unknown_clock_allows_future_sample_time_without_measurement(self) -> None:
        # Constructed regression for the unchanged producer's future-time guard.
        document = json.loads((MQG2_FIXTURE_DIR / "source-identity-unavailable.json").read_bytes())
        document["source"]["boot_id"] = TEST_ONLY_BASE["source"]["boot_id"]
        document["source"]["monotonic_us"] = str(int(document["source"]["response_monotonic_us"]) + 1)
        self.assertEqual(self.check(document), "MQG2")
        ready = copy.deepcopy(TEST_ONLY_BASE)
        ready["source"]["monotonic_us"] = document["source"]["monotonic_us"]
        self.rejected(ready, "response time precedes source time")

    def test_preheat_unconfirmed_status_preserves_null_ppm(self) -> None:
        doc = copy.deepcopy(TEST_ONLY_BASE)
        doc["quality"] = "UNCALIBRATED"
        doc["reason"] = "PREHEAT_UNCONFIRMED"
        doc["reference"]["heating_duration_us"] = "172800000000"
        doc["reference"]["preparation_confirmed"] = False
        doc["reference"]["preparation_method"] = "UNKNOWN"
        doc["nh3"]["ppm"] = None
        doc["nh3"]["status"] = "PREHEAT_UNCONFIRMED"
        self.assertEqual(self.check(doc), "MQG2")


if __name__ == "__main__":
    unittest.main()
