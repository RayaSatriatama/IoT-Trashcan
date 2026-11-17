package devicecontrol

import (
	"bytes"
	"encoding/json"
	"io"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

const mq135NormalizedCurveHash = "sha256:4aa984238efa7d253b77ac828babd40e3a8f10ef14a3a86f8b9f1da0e6dd414b"

func mq135MQG2Object(value any, fields string) (map[string]any, bool) {
	object, ok := value.(map[string]any)
	if !ok {
		return nil, false
	}
	names := strings.Fields(fields)
	if len(object) != len(names) {
		return nil, false
	}
	for _, name := range names {
		if _, exists := object[name]; !exists {
			return nil, false
		}
	}
	return object, true
}

func mq135MQG2Enum(value any, choices string) bool {
	text, ok := value.(string)
	return ok && strings.Contains("|"+choices+"|", "|"+text+"|")
}

func mq135MQG2Number(value any, minimum, maximum float64, integer bool) (float64, bool) {
	number, ok := value.(json.Number)
	if !ok {
		return 0, false
	}
	if integer {
		if _, err := number.Int64(); err != nil {
			return 0, false
		}
	}
	parsed, err := number.Float64()
	if err != nil || math.IsNaN(parsed) || math.IsInf(parsed, 0) || parsed < minimum || parsed > maximum {
		return 0, false
	}
	return parsed, true
}

func mq135MQG2NullableNumber(value any, minimum, maximum float64, integer bool) (float64, bool) {
	if value == nil {
		return 0, true
	}
	return mq135MQG2Number(value, minimum, maximum, integer)
}

func mq135MQG2Uint(value any, minimum, maximum uint64) (uint64, bool) {
	text, ok := value.(string)
	if !ok || text == "" {
		return 0, false
	}
	number, err := strconv.ParseUint(text, 10, 64)
	return number, err == nil && strconv.FormatUint(number, 10) == text && number >= minimum && number <= maximum
}

func mq135MQG2NullableUint(value any, minimum, maximum uint64) (uint64, bool) {
	if value == nil {
		return 0, true
	}
	return mq135MQG2Uint(value, minimum, maximum)
}

func mq135MQG2NullableInt64(value any) (int64, bool) {
	if value == nil {
		return 0, true
	}
	text, ok := value.(string)
	if !ok || text == "" {
		return 0, false
	}
	number, err := strconv.ParseInt(text, 10, 64)
	return number, err == nil && strconv.FormatInt(number, 10) == text
}

func mq135MQG2Label(value any) bool {
	text, ok := value.(string)
	return ok && mq135Label(text)
}

func mq135MQG2Failure(value any) bool {
	text, ok := value.(string)
	if !ok || len(text) == 0 || len(text) > 32 || text[0] < 'A' || text[0] > 'Z' {
		return false
	}
	for _, char := range text[1:] {
		if !(char >= 'A' && char <= 'Z' || char >= '0' && char <= '9' || char == '_') {
			return false
		}
	}
	return true
}

func validMq135GasResultMQG2(payload []byte) bool {
	if !utf8.Valid(payload) {
		return false
	}
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.UseNumber()
	var value any
	if decoder.Decode(&value) != nil {
		return false
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		return false
	}
	root, ok := mq135MQG2Object(value, "schema sensor quality reason adc electrical response nh3 profiles source reference")
	if !ok || root["schema"] != "MQG2" || root["sensor"] != "mq135" ||
		!mq135MQG2Enum(root["quality"], "READY|CALIBRATING|UNCALIBRATED|STALE|INVALID|OUT_OF_RANGE|DISCONNECTED|TIMEOUT|UNAVAILABLE|WARMING_UP|HEATER_PHASE|CRC_ERROR") ||
		!mq135MQG2Enum(root["reason"], "NO_NORMALIZED_PROFILE|NORMALIZED_PROFILE_INVALID|ADC_CALIBRATION_UNAVAILABLE|NO_SOURCE|SOURCE_IDENTITY_INVALID|SOURCE_NOT_READY|SOURCE_CLOCK_UNKNOWN|SOURCE_STALE|ADC_PROFILE_CHANGED|AO_OUT_OF_RANGE|BASELINE_MISSING|IN_RANGE|BELOW_RANGE|ABOVE_RANGE|INVALID_RATIO|CURVE_INVALID|CURVE_UNAVAILABLE|REFERENCE_NOT_CLEAN_AIR|PREHEAT_UNCONFIRMED") {
		return false
	}

	adc, ok := mq135MQG2Object(root["adc"], "raw_counts available pin_mv ao_mv method same_sample failure resolution_bits attenuation_db adc_unit adc_channel")
	if !ok {
		return false
	}
	rawCounts, ok := mq135MQG2NullableNumber(adc["raw_counts"], 0, 4095, true)
	if !ok {
		return false
	}
	pinMV, ok := mq135MQG2NullableNumber(adc["pin_mv"], 0, math.MaxFloat64, false)
	if !ok {
		return false
	}
	_, ok = mq135MQG2NullableNumber(adc["ao_mv"], 0, math.MaxFloat64, false)
	if !ok {
		return false
	}
	available, availableOK := adc["available"].(bool)
	sameSample, sameSampleOK := adc["same_sample"].(bool)
	method, methodOK := adc["method"].(string)
	failure, failureOK := adc["failure"].(string)
	if !availableOK || !sameSampleOK || !methodOK || !failureOK ||
		(method != "EFUSE_TP_FIT" && method != "UNAVAILABLE") || !mq135MQG2Failure(failure) ||
		!mq135MQG2NullableNumberFits(adc["resolution_bits"], 1, 32) ||
		!mq135MQG2NullableNumberFits(adc["attenuation_db"], 0, 255) ||
		!mq135MQG2NullableNumberFits(adc["adc_unit"], 0, 255) ||
		!mq135MQG2NullableNumberFits(adc["adc_channel"], 0, 255) {
		return false
	}
	if available {
		if method != "EFUSE_TP_FIT" || !sameSample || failure != "NONE" ||
			adc["resolution_bits"] != json.Number("12") || adc["attenuation_db"] != json.Number("11") ||
			adc["adc_unit"] != json.Number("1") || adc["adc_channel"] != json.Number("4") ||
			adc["raw_counts"] == nil || rawCounts <= 0 || rawCounts >= 4095 || adc["pin_mv"] == nil || pinMV <= 0 {
			return false
		}
	} else if failure == "NONE" || adc["pin_mv"] != nil || adc["ao_mv"] != nil || (method == "UNAVAILABLE" &&
		(adc["resolution_bits"] != nil || adc["attenuation_db"] != nil || adc["adc_unit"] != nil || adc["adc_channel"] != nil)) {
		return false
	}

	electrical, ok := mq135MQG2Object(root["electrical"], "circuit_mv divider_top_ohm divider_bottom_ohm topology hardware_confirmed hardware_identity")
	if !ok || !mq135MQG2NullableNumberFits(electrical["circuit_mv"], 1, math.MaxUint32) ||
		!mq135MQG2NullableNumberFits(electrical["divider_top_ohm"], 1, math.MaxUint32) ||
		!mq135MQG2NullableNumberFits(electrical["divider_bottom_ohm"], 1, math.MaxUint32) ||
		!mq135MQG2NullableIntegerChoice(electrical["topology"], "1|2") {
		return false
	}
	hardwareConfirmed, hardwareOK := electrical["hardware_confirmed"].(bool)
	if !hardwareOK || !mq135MQG2NullableLabel(electrical["hardware_identity"]) {
		return false
	}
	circuitPresent := electrical["circuit_mv"] != nil && electrical["divider_top_ohm"] != nil &&
		electrical["divider_bottom_ohm"] != nil && electrical["topology"] != nil
	if hardwareConfirmed && (!circuitPresent || electrical["hardware_identity"] == nil) {
		return false
	}
	if adc["ao_mv"] != nil && (!available || adc["pin_mv"] == nil || !circuitPresent) {
		return false
	}

	response, ok := mq135MQG2Object(root["response"], "q q0 ratio reference_kind baseline_valid")
	if !ok || !mq135MQG2NullableNumberFits(response["q"], 0, math.MaxFloat64) ||
		!mq135MQG2NullableNumberFits(response["q0"], 0, math.MaxFloat64) ||
		!mq135MQG2NullableNumberFits(response["ratio"], 0, math.MaxFloat64) ||
		!mq135MQG2Enum(response["reference_kind"], "NONE|OPERATIONAL|CLEAN_AIR_CONFIRMED") {
		return false
	}
	baselineValid, baselineOK := response["baseline_valid"].(bool)
	if !baselineOK || (response["q"] != nil && !mq135MQG2PositiveNumber(response["q"])) ||
		(response["q0"] != nil && !mq135MQG2PositiveNumber(response["q0"])) ||
		(response["ratio"] != nil && !mq135MQG2PositiveNumber(response["ratio"])) ||
		baselineValid != (response["q0"] != nil) {
		return false
	}

	nh3, ok := mq135MQG2Object(root["nh3"], "ppm status curve_id curve_version source_sha256 method production_eligible")
	if !ok || !mq135MQG2NullableNumberFits(nh3["ppm"], 10, 1000) ||
		!mq135MQG2Enum(nh3["status"], "UNCALIBRATED|REFERENCE_NOT_CLEAN_AIR|PREHEAT_UNCONFIRMED|ADC_CALIBRATION_UNAVAILABLE|SOURCE_NOT_READY|SOURCE_STALE|PROFILE_INVALID|IN_RANGE|BELOW_RANGE|ABOVE_RANGE|CURVE_UNAVAILABLE|CURVE_INVALID|INVALID_RATIO") ||
		!mq135MQG2NullableIntegerChoice(nh3["curve_id"], "20040") || !mq135MQG2NullableIntegerChoice(nh3["curve_version"], "2") ||
		!mq135MQG2NullableChoice(nh3["source_sha256"], mq135NormalizedCurveHash) ||
		nh3["method"] != "PIECEWISE_LOG_LOG" || nh3["production_eligible"] != false {
		return false
	}

	profiles, ok := mq135MQG2Object(root["profiles"], "state version hash previous_version previous_hash adc_version adc_hash")
	if !ok || !mq135MQG2Enum(profiles["state"], "MISSING|INVALID|TEST_ONLY|VALID") ||
		!mq135MQG2NullableProfileVersion(profiles["version"]) || !mq135MQG2NullableHash(profiles["hash"]) ||
		!mq135MQG2NullableProfileVersion(profiles["previous_version"]) || !mq135MQG2NullableHash(profiles["previous_hash"]) ||
		!mq135MQG2NullableProfileVersion(profiles["adc_version"]) || !mq135MQG2NullableHash(profiles["adc_hash"]) ||
		!mq135MQG2Pair(profiles["version"], profiles["hash"]) ||
		!mq135MQG2Pair(profiles["previous_version"], profiles["previous_hash"]) ||
		!mq135MQG2Pair(profiles["adc_version"], profiles["adc_hash"]) {
		return false
	}
	profileState, _ := profiles["state"].(string)
	if (profileState == "MISSING" && profiles["version"] != nil) ||
		((profileState == "TEST_ONLY" || profileState == "VALID") && profiles["version"] == nil) {
		return false
	}

	source, ok := mq135MQG2Object(root["source"], "boot_id sequence monotonic_us response_monotonic_us age_us max_age_us fresh quality")
	if !ok || !mq135MQG2NullableBootID(source["boot_id"]) ||
		!mq135MQG2NullableUintFits(source["sequence"]) || !mq135MQG2NullableUintFits(source["monotonic_us"]) ||
		!mq135MQG2UintFits(source["response_monotonic_us"]) || !mq135MQG2NullableUintFits(source["age_us"]) ||
		!mq135MQG2NullableUintFits(source["max_age_us"]) || !mq135MQG2NullableBool(source["fresh"]) ||
		!mq135MQG2NullableChoice(source["quality"], "READY|CALIBRATING|WARMING_UP|HEATER_PHASE|UNCALIBRATED|STALE|TIMEOUT|DISCONNECTED|CRC_ERROR|OUT_OF_RANGE|INVALID|UNAVAILABLE") {
		return false
	}
	responseTime, _ := mq135MQG2Uint(source["response_monotonic_us"], 0, math.MaxUint64)
	sourceSequence, sourceSequenceOK := mq135MQG2NullableUint(source["sequence"], 0, math.MaxUint64)
	sourceMono, sourceMonoOK := mq135MQG2NullableUint(source["monotonic_us"], 0, math.MaxUint64)
	age, ageOK := mq135MQG2NullableUint(source["age_us"], 0, math.MaxUint64)
	maxAge, maxAgeOK := mq135MQG2NullableUint(source["max_age_us"], 0, math.MaxUint64)
	reasonText, _ := root["reason"].(string)
	if !sourceSequenceOK || !sourceMonoOK || !ageOK || !maxAgeOK {
		return false
	}
	identityFields := []bool{source["sequence"] != nil, source["boot_id"] != nil, source["monotonic_us"] != nil}
	identityCount := 0
	for _, present := range identityFields {
		if present {
			identityCount++
		}
	}
	partialBootIdentity := source["boot_id"] == nil && source["sequence"] != nil && source["monotonic_us"] != nil &&
		source["quality"] != nil && (reasonText == "SOURCE_IDENTITY_INVALID" || reasonText == "SOURCE_CLOCK_UNKNOWN")
	if identityCount != 0 && identityCount != len(identityFields) && !partialBootIdentity {
		return false
	}
	if identityCount == 0 {
		if source["age_us"] != nil || source["max_age_us"] != nil || source["fresh"] != nil ||
			(source["quality"] != nil && reasonText != "SOURCE_IDENTITY_INVALID" && reasonText != "SOURCE_CLOCK_UNKNOWN") {
			return false
		}
	} else {
		clockUnavailable := (reasonText == "SOURCE_IDENTITY_INVALID" || reasonText == "SOURCE_CLOCK_UNKNOWN") &&
			source["age_us"] == nil && source["max_age_us"] == nil && source["fresh"] == nil
		if sourceSequence == 0 || (responseTime < sourceMono && !clockUnavailable) ||
			(source["age_us"] != nil && age != responseTime-sourceMono) {
			return false
		}
		if source["age_us"] != nil && source["max_age_us"] != nil {
			fresh, freshOK := source["fresh"].(bool)
			if !freshOK || fresh != (age <= maxAge) {
				return false
			}
		} else if source["fresh"] != nil {
			return false
		}
	}

	reference, ok := mq135MQG2Object(root["reference"], "environment temperature_centi_c humidity_centi_pct conditions_source heating_duration_us preparation_confirmed preparation_method sample_count variation_percent calibration_epoch_ms calibration_time_quality")
	if !ok || !mq135MQG2NullableLabel(reference["environment"]) ||
		!mq135MQG2NullableNumberFits(reference["temperature_centi_c"], math.MinInt32, math.MaxInt32, true) ||
		!mq135MQG2NullableNumberFits(reference["humidity_centi_pct"], 0, 10000, true) ||
		!mq135MQG2Enum(reference["conditions_source"], "UNKNOWN|DECLARED") ||
		!mq135MQG2NullableUintFits(reference["heating_duration_us"]) ||
		!mq135MQG2NullableNumberFits(reference["sample_count"], 0, 64, true) ||
		!mq135MQG2NullableNumberFits(reference["variation_percent"], 0, math.MaxFloat64) ||
		!mq135MQG2NullableInt64Fits(reference["calibration_epoch_ms"]) ||
		!mq135MQG2Enum(reference["calibration_time_quality"], "SYNCHRONIZED|ESTIMATED|UNSYNCED") ||
		!mq135MQG2Boolean(reference["preparation_confirmed"]) ||
		!mq135MQG2Enum(reference["preparation_method"], "UNKNOWN|OPERATOR_DECLARED") {
		return false
	}
	preparationConfirmed, _ := reference["preparation_confirmed"].(bool)
	preparationMethod, _ := reference["preparation_method"].(string)
	declaredConditions := reference["environment"] != nil || reference["temperature_centi_c"] != nil || reference["humidity_centi_pct"] != nil
	conditionsSource, _ := reference["conditions_source"].(string)
	calibrationTimeQuality, _ := reference["calibration_time_quality"].(string)
	calibrationEpoch, _ := mq135MQG2NullableInt64(reference["calibration_epoch_ms"])
	if (preparationConfirmed && preparationMethod != "OPERATOR_DECLARED") ||
		(declaredConditions != (conditionsSource == "DECLARED")) ||
		(calibrationTimeQuality == "SYNCHRONIZED" && reference["calibration_epoch_ms"] == nil) ||
		(calibrationTimeQuality == "UNSYNCED" && reference["calibration_epoch_ms"] != nil) {
		return false
	}
	_ = calibrationEpoch // The canonical signed/int64 bound was checked above.

	curvePresent := nh3["curve_id"] == json.Number("20040") && nh3["curve_version"] == json.Number("2") && nh3["source_sha256"] == mq135NormalizedCurveHash
	curveAbsent := nh3["curve_id"] == nil && nh3["curve_version"] == nil && nh3["source_sha256"] == nil
	if !curvePresent && !curveAbsent {
		return false
	}
	if baselineValid && profileState != "TEST_ONLY" && profileState != "VALID" {
		return false
	}
	if response["reference_kind"] == "NONE" && baselineValid {
		return false
	}
	if response["q"] != nil && (!available || adc["ao_mv"] == nil || source["fresh"] != true || source["quality"] != "READY") {
		return false
	}
	if response["ratio"] != nil {
		if response["q"] == nil || response["q0"] == nil || !baselineValid {
			return false
		}
		q, _ := mq135MQG2Number(response["q"], 0, math.MaxFloat64, false)
		q0, _ := mq135MQG2Number(response["q0"], 0, math.MaxFloat64, false)
		ratio, _ := mq135MQG2Number(response["ratio"], 0, math.MaxFloat64, false)
		derived := q / q0
		if math.Abs(ratio-derived) > math.Max(1e-9, 1e-6*math.Max(math.Abs(ratio), math.Abs(derived))) {
			return false
		}
	} else if response["q"] != nil && baselineValid && (profileState == "TEST_ONLY" || profileState == "VALID") &&
		root["reason"] != "ADC_PROFILE_CHANGED" && root["reason"] != "BASELINE_MISSING" {
		return false
	}

	quality, _ := root["quality"].(string)
	reason, _ := root["reason"].(string)
	status, _ := nh3["status"].(string)
	if status == "IN_RANGE" || status == "BELOW_RANGE" || status == "ABOVE_RANGE" {
		if reason != status || quality != "READY" {
			return false
		}
	}
	ppm := nh3["ppm"]
	if ppm != nil && status != "IN_RANGE" {
		return false
	}
	if reason == "SOURCE_IDENTITY_INVALID" || reason == "SOURCE_CLOCK_UNKNOWN" {
		if status != "SOURCE_NOT_READY" || quality != "INVALID" || source["quality"] == nil ||
			adc["ao_mv"] != nil || response["q"] != nil || response["ratio"] != nil || ppm != nil {
			return false
		}
		if reason == "SOURCE_CLOCK_UNKNOWN" && (source["quality"] != "READY" ||
			source["age_us"] != nil || source["max_age_us"] != nil || source["fresh"] != nil) {
			return false
		}
	}
	ratioValue, ratioPresent := mq135MQG2Number(response["ratio"], 0, math.MaxFloat64, false)
	if status == "IN_RANGE" && (!ratioPresent || ratioValue < 0.0893 || ratioValue > 0.6559 || ppm == nil) {
		return false
	}
	if status == "BELOW_RANGE" && (!ratioPresent || ratioValue <= 0.6559 || ppm != nil) {
		return false
	}
	if status == "ABOVE_RANGE" && (!ratioPresent || ratioValue >= 0.0893 || ppm != nil) {
		return false
	}

	if ppm != nil {
		heating, heatingOK := mq135MQG2NullableUint(reference["heating_duration_us"], 0, math.MaxUint64)
		sampleCount, sampleCountOK := mq135MQG2NullableNumber(reference["sample_count"], 0, 64, true)
		if !heatingOK || !sampleCountOK || quality != "READY" || reason != "IN_RANGE" || !available ||
			source["fresh"] != true || source["quality"] != "READY" || response["q"] == nil || response["q0"] == nil ||
			response["ratio"] == nil || !baselineValid || response["reference_kind"] != "CLEAN_AIR_CONFIRMED" ||
			(profileState != "TEST_ONLY" && profileState != "VALID") || !curvePresent || !preparationConfirmed ||
			preparationMethod != "OPERATOR_DECLARED" || reference["heating_duration_us"] == nil || heating <= 172800000000 ||
			reference["sample_count"] == nil || sampleCount < 3 || reference["variation_percent"] == nil {
			return false
		}
	}

	if status == "SOURCE_STALE" && (source["fresh"] != false || response["q"] != nil || response["ratio"] != nil) {
		return false
	}
	if status == "SOURCE_NOT_READY" && ((reason != "SOURCE_IDENTITY_INVALID" && source["quality"] == "READY" && source["fresh"] != nil) || response["q"] != nil || response["ratio"] != nil) {
		return false
	}
	if status == "ADC_CALIBRATION_UNAVAILABLE" && available {
		return false
	}
	if status == "REFERENCE_NOT_CLEAN_AIR" && (response["reference_kind"] == "CLEAN_AIR_CONFIRMED" || response["ratio"] == nil || ppm != nil) {
		return false
	}
	if status == "PREHEAT_UNCONFIRMED" {
		heating, heatingOK := mq135MQG2NullableUint(reference["heating_duration_us"], 0, math.MaxUint64)
		prepared := preparationConfirmed && preparationMethod == "OPERATOR_DECLARED" && heatingOK && reference["heating_duration_us"] != nil && heating > 172800000000
		if response["reference_kind"] != "CLEAN_AIR_CONFIRMED" || response["ratio"] == nil || ppm != nil || prepared {
			return false
		}
	}
	if reason != "SOURCE_IDENTITY_INVALID" && (source["quality"] == "WARMING_UP" || source["quality"] == "HEATER_PHASE" || source["quality"] == "CRC_ERROR") {
		if quality != source["quality"] || status != "SOURCE_NOT_READY" || reason != "SOURCE_NOT_READY" {
			return false
		}
	}
	if reason != "SOURCE_IDENTITY_INVALID" && status == "SOURCE_NOT_READY" && source["quality"] != nil && source["quality"] != "READY" && quality != source["quality"] {
		return false
	}
	if !mq135MQG2StatusMapping(status, reason, quality) {
		return false
	}

	expectedStatus := ""
	switch {
	case profileState == "INVALID" || reason == "ADC_PROFILE_CHANGED":
		expectedStatus = "PROFILE_INVALID"
	case profileState == "MISSING":
		expectedStatus = "UNCALIBRATED"
	case !available:
		expectedStatus = "ADC_CALIBRATION_UNAVAILABLE"
	case reason == "SOURCE_IDENTITY_INVALID":
		expectedStatus = "SOURCE_NOT_READY"
	case source["quality"] != "READY" || source["sequence"] == nil || source["fresh"] == nil:
		expectedStatus = "SOURCE_NOT_READY"
	case source["fresh"] == false:
		expectedStatus = "SOURCE_STALE"
	case response["q"] == nil:
		expectedStatus = "INVALID_RATIO"
	case response["ratio"] == nil:
		expectedStatus = "UNCALIBRATED"
	case status == "CURVE_UNAVAILABLE" || status == "CURVE_INVALID":
		expectedStatus = status
	case !curvePresent:
		if status != "CURVE_UNAVAILABLE" && status != "CURVE_INVALID" {
			return false
		}
		expectedStatus = status
	case response["reference_kind"] != "CLEAN_AIR_CONFIRMED":
		expectedStatus = "REFERENCE_NOT_CLEAN_AIR"
	case !preparationConfirmed || preparationMethod != "OPERATOR_DECLARED" || reference["heating_duration_us"] == nil:
		expectedStatus = "PREHEAT_UNCONFIRMED"
	default:
		heating, _ := mq135MQG2NullableUint(reference["heating_duration_us"], 0, math.MaxUint64)
		if heating <= 172800000000 {
			expectedStatus = "PREHEAT_UNCONFIRMED"
		} else if ratioValue > 0.6559 {
			expectedStatus = "BELOW_RANGE"
		} else if ratioValue < 0.0893 {
			expectedStatus = "ABOVE_RANGE"
		} else {
			expectedStatus = "IN_RANGE"
		}
	}
	return status == expectedStatus
}

func mq135MQG2NullableNumberFits(value any, minimum, maximum float64, integer ...bool) bool {
	whole := len(integer) != 0 && integer[0]
	_, ok := mq135MQG2NullableNumber(value, minimum, maximum, whole)
	return ok
}

func mq135MQG2NullableChoice(value any, choices string) bool {
	if value == nil {
		return true
	}
	return mq135MQG2Enum(value, choices)
}

func mq135MQG2NullableIntegerChoice(value any, choices string) bool {
	if value == nil {
		return true
	}
	number, ok := mq135MQG2Number(value, 0, math.MaxUint32, true)
	return ok && strings.Contains("|"+choices+"|", "|"+strconv.FormatUint(uint64(number), 10)+"|")
}

func mq135MQG2Boolean(value any) bool {
	_, ok := value.(bool)
	return ok
}

func mq135MQG2NullableLabel(value any) bool { return value == nil || mq135MQG2Label(value) }
func mq135MQG2PositiveNumber(value any) bool {
	number, ok := mq135MQG2Number(value, 0, math.MaxFloat64, false)
	return ok && number > 0
}

func mq135MQG2Pair(left, right any) bool { return (left == nil) == (right == nil) }

func mq135MQG2NullableProfileVersion(value any) bool {
	if value == nil {
		return true
	}
	_, ok := mq135MQG2Uint(value, 1, math.MaxUint32)
	return ok
}

func mq135MQG2NullableHash(value any) bool {
	if value == nil {
		return true
	}
	text, ok := value.(string)
	return ok && validSHA256(text)
}

func mq135MQG2NullableBootID(value any) bool {
	if value == nil {
		return true
	}
	text, ok := value.(string)
	if !ok || len(text) != 32 || strings.ToLower(text) != text {
		return false
	}
	for _, char := range text {
		if !(char >= '0' && char <= '9' || char >= 'a' && char <= 'f') {
			return false
		}
	}
	return true
}

func mq135MQG2NullableUintFits(value any) bool {
	if value == nil {
		return true
	}
	_, ok := mq135MQG2Uint(value, 0, math.MaxUint64)
	return ok
}

func mq135MQG2UintFits(value any) bool {
	_, ok := mq135MQG2Uint(value, 0, math.MaxUint64)
	return ok
}

func mq135MQG2NullableBool(value any) bool {
	if value == nil {
		return true
	}
	_, ok := value.(bool)
	return ok
}

func mq135MQG2NullableInt64Fits(value any) bool {
	_, ok := mq135MQG2NullableInt64(value)
	return ok
}

func mq135MQG2StatusMapping(status, reason, quality string) bool {
	switch status {
	case "PROFILE_INVALID":
		return (reason == "NORMALIZED_PROFILE_INVALID" || reason == "ADC_PROFILE_CHANGED") && quality == "INVALID"
	case "UNCALIBRATED":
		return (reason == "NO_NORMALIZED_PROFILE" || reason == "BASELINE_MISSING") && quality == "UNCALIBRATED"
	case "ADC_CALIBRATION_UNAVAILABLE":
		return reason == "ADC_CALIBRATION_UNAVAILABLE" && quality == "UNAVAILABLE"
	case "SOURCE_STALE":
		return reason == "SOURCE_STALE" && quality == "STALE"
	case "SOURCE_NOT_READY":
		switch reason {
		case "NO_SOURCE":
			return quality == "DISCONNECTED"
		case "SOURCE_IDENTITY_INVALID", "SOURCE_CLOCK_UNKNOWN":
			return quality == "INVALID"
		case "SOURCE_NOT_READY":
			return quality != "READY"
		default:
			return false
		}
	case "INVALID_RATIO":
		return (reason == "AO_OUT_OF_RANGE" || reason == "INVALID_RATIO") && (quality == "OUT_OF_RANGE" || quality == "INVALID")
	case "CURVE_UNAVAILABLE":
		return reason == "CURVE_UNAVAILABLE" && quality == "UNAVAILABLE"
	case "CURVE_INVALID":
		return reason == "CURVE_INVALID" && quality == "INVALID"
	case "REFERENCE_NOT_CLEAN_AIR":
		return reason == "REFERENCE_NOT_CLEAN_AIR" && quality == "UNCALIBRATED"
	case "PREHEAT_UNCONFIRMED":
		return reason == "PREHEAT_UNCONFIRMED" && quality == "UNCALIBRATED"
	default:
		return true
	}
}
