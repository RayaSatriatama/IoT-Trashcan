package devicecontrol

import (
	"encoding/hex"
	"encoding/json"
	"math"
	"strconv"
	"strings"
)

// MQG1 is control evidence, never a telemetry-v3 sample or a gas concentration.
const mq135ResultFields = "schema sensor quality reason raw_counts adc_method adc_pin_mv ao_mv rs_ohm r0_ohm rs_over_r0 nh3_ppm nh3_status profile_state profile_version profile_hash previous_profile_version previous_profile_hash adc_profile_version adc_profile_hash source_sequence source_boot_id source_monotonic_us response_monotonic_us age_us max_age_us fresh hardware_identity reference_environment reference_temperature_centi_c reference_humidity_centi_pct calibration_epoch_ms calibration_time_quality sample_count variation_percent circuit_mv module_load_ohm divider_top_ohm divider_bottom_ohm topology"

func mq135Choice(value any, choices string) bool {
	text, ok := value.(string)
	return ok && text != "" && strings.Contains("|"+choices+"|", "|"+text+"|")
}
func mq135Uint(value any, bits int) (uint64, bool) {
	text, ok := value.(string)
	if !ok || text == "" {
		return 0, false
	}
	n, err := strconv.ParseUint(text, 10, bits)
	return n, err == nil && strconv.FormatUint(n, 10) == text
}
func mq135Number(value any, minimum, maximum float64, integer bool) bool {
	n, ok := value.(float64)
	return ok && !math.IsNaN(n) && !math.IsInf(n, 0) && n >= minimum && n <= maximum && (!integer || math.Trunc(n) == n)
}
func mq135Label(value any) bool {
	text, ok := value.(string)
	if !ok || len(text) == 0 || len(text) > 31 {
		return false
	}
	for _, c := range text {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.ContainsRune("_-.:", c)) {
			return false
		}
	}
	return true
}
func validMq135GasResult(payload []byte) bool {
	if len(payload) == 0 || len(payload) > MaxPayloadBytes || rejectDuplicateJSONKeys(payload) != nil {
		return false
	}
	var header struct {
		Schema string `json:"schema"`
	}
	if json.Unmarshal(payload, &header) != nil {
		return false
	}
	if header.Schema == "MQG2" {
		return validMq135GasResultMQG2(payload)
	}
	if header.Schema != "MQG1" {
		return false
	}
	var v map[string]any
	if json.Unmarshal(payload, &v) != nil {
		return false
	}
	fields := strings.Fields(mq135ResultFields)
	if len(v) != len(fields) {
		return false
	}
	for _, key := range fields {
		if _, ok := v[key]; !ok {
			return false
		}
	}
	if v["schema"] != "MQG1" || v["sensor"] != "mq135" || v["adc_method"] != "PROFILE_LINEAR" ||
		v["nh3_ppm"] != nil || v["nh3_status"] != "UNCALIBRATED" {
		return false
	}
	if !mq135Choice(v["quality"], "READY|CALIBRATING|WARMING_UP|HEATER_PHASE|UNCALIBRATED|STALE|TIMEOUT|DISCONNECTED|CRC_ERROR|OUT_OF_RANGE|INVALID") ||
		!mq135Choice(v["reason"], "NONE|GAS_PROFILE_MISSING|NO_SOURCE|RAW_MISSING|SOURCE_IDENTITY_INVALID|ADC_UNVERIFIED|ADC_PROFILE_CHANGED|SOURCE_NOT_READY|SOURCE_CLOCK_UNKNOWN|SOURCE_STALE|GAS_PROFILE_INVALID|REFERENCE_PENDING|ADC_SATURATED|DIVIDER_MISMATCH|AO_OUT_OF_RANGE|ELECTRICAL_UNVERIFIED|ELECTRICAL_OR_SOURCE_INVALID") ||
		!mq135Choice(v["profile_state"], "MISSING|INVALID|TEST_ONLY|VALID") ||
		!mq135Choice(v["calibration_time_quality"], "UNSYNCED|SYNCHRONIZED|ESTIMATED") {
		return false
	}
	for _, key := range []string{"raw_counts", "adc_pin_mv", "ao_mv", "rs_ohm", "r0_ohm", "rs_over_r0", "variation_percent", "circuit_mv", "module_load_ohm", "divider_top_ohm", "divider_bottom_ohm"} {
		if v[key] != nil && !mq135Number(v[key], 0, math.MaxFloat64, false) {
			return false
		}
	}
	if v["raw_counts"] != nil && !mq135Number(v["raw_counts"], 0, 65535, true) {
		return false
	}
	for _, key := range []string{"rs_ohm", "r0_ohm", "rs_over_r0", "circuit_mv", "module_load_ohm", "divider_top_ohm", "divider_bottom_ohm"} {
		if v[key] != nil && v[key].(float64) <= 0 {
			return false
		}
	}
	for _, pair := range [][2]string{{"profile_version", "profile_hash"}, {"previous_profile_version", "previous_profile_hash"}, {"adc_profile_version", "adc_profile_hash"}} {
		if (v[pair[0]] == nil) != (v[pair[1]] == nil) {
			return false
		}
		if v[pair[0]] != nil {
			n, ok := mq135Uint(v[pair[0]], 32)
			hash, str := v[pair[1]].(string)
			if !ok || n == 0 || !str || !validSHA256(hash) {
				return false
			}
		}
	}
	for _, key := range []string{"source_sequence", "source_monotonic_us", "response_monotonic_us", "age_us", "max_age_us"} {
		if v[key] != nil {
			if _, ok := mq135Uint(v[key], 64); !ok {
				return false
			}
		}
	}
	if v["response_monotonic_us"] == nil {
		return false
	}
	if v["source_boot_id"] != nil {
		text, ok := v["source_boot_id"].(string)
		if !ok || len(text) != 32 || strings.ToLower(text) != text {
			return false
		}
		if _, err := hex.DecodeString(text); err != nil {
			return false
		}
	}
	if (v["source_sequence"] == nil) != (v["source_monotonic_us"] == nil) {
		return false
	}
	if v["fresh"] != nil {
		fresh, ok := v["fresh"].(bool)
		age, ageOK := mq135Uint(v["age_us"], 64)
		budget, budgetOK := mq135Uint(v["max_age_us"], 64)
		source, sourceOK := mq135Uint(v["source_monotonic_us"], 64)
		response, responseOK := mq135Uint(v["response_monotonic_us"], 64)
		if !ok || !ageOK || !budgetOK || budget == 0 || !sourceOK || !responseOK || v["source_boot_id"] == nil || response < source || response-source != age || fresh != (age <= budget) {
			return false
		}
	} else if v["max_age_us"] != nil {
		return false
	}
	for _, key := range []string{"hardware_identity", "reference_environment"} {
		if v[key] != nil && !mq135Label(v[key]) {
			return false
		}
	}
	if v["reference_temperature_centi_c"] != nil && !mq135Number(v["reference_temperature_centi_c"], math.MinInt32, math.MaxInt32, true) {
		return false
	}
	if v["reference_humidity_centi_pct"] != nil && !mq135Number(v["reference_humidity_centi_pct"], 0, 10000, true) {
		return false
	}
	if v["sample_count"] != nil && !mq135Number(v["sample_count"], 3, 64, true) {
		return false
	}
	if v["topology"] != nil && !mq135Number(v["topology"], 1, 2, true) {
		return false
	}
	if v["calibration_epoch_ms"] != nil {
		text, ok := v["calibration_epoch_ms"].(string)
		n, err := strconv.ParseInt(text, 10, 64)
		if !ok || err != nil || strconv.FormatInt(n, 10) != text || v["calibration_time_quality"] == "UNSYNCED" {
			return false
		}
	}
	profile := v["profile_state"] == "VALID" || v["profile_state"] == "TEST_ONLY"
	for _, key := range []string{"profile_version", "r0_ohm", "sample_count", "variation_percent"} {
		if (v[key] != nil) != profile {
			return false
		}
	}
	if v["quality"] == "READY" && (v["reason"] != "NONE" || !profile || v["fresh"] != true || v["rs_over_r0"] == nil || v["adc_profile_hash"] == nil) {
		return false
	}
	if v["rs_over_r0"] != nil {
		if v["quality"] != "READY" || v["rs_ohm"] == nil || v["r0_ohm"] == nil || v["raw_counts"] == nil || v["ao_mv"] == nil || v["adc_pin_mv"] == nil {
			return false
		}
		ratio := v["rs_over_r0"].(float64)
		// Serialization precision only, not a scientific accuracy claim.
		if math.Abs(v["rs_ohm"].(float64)/v["r0_ohm"].(float64)-ratio) > 1e-5*math.Max(1, ratio) {
			return false
		}
	}
	return true
}
