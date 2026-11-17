package devicecontrol

import (
	"bytes"
	"encoding/json"
	"math"
	"strconv"
)

const mq135CriteriaFields = "samples min_duration_ms max_age_ms max_variation_milli_percent circuit_mv module_load_ohm divider_top_ohm divider_bottom_ohm topology hardware_confirmed preparation_confirmed reference_confirmed reference_temperature_centi_c reference_humidity_centi_pct heating_duration_us hardware_identity reference_environment"

// Flatten only explicitly declared web form groups. Duplicate names, device ADC
// coefficients, fitted R0 and unknown values cannot cross this boundary.
func mq135CommandPayload(operation string, payload []byte) ([]byte, error) {
	if len(payload) == 0 || len(payload) > MaxPayloadBytes || rejectDuplicateJSONKeys(payload) != nil {
		return nil, ErrInvalid
	}
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.UseNumber()
	var input map[string]any
	if decoder.Decode(&input) != nil || input == nil {
		return nil, ErrInvalid
	}
	flat := map[string]any{}
	for key, value := range input {
		if key == "parameters" || key == "references" || key == "criteria" {
			group, ok := value.(map[string]any)
			if !ok {
				return nil, ErrInvalid
			}
			for field, item := range group {
				if _, exists := flat[field]; exists {
					return nil, ErrInvalid
				}
				flat[field] = item
			}
		} else {
			if _, exists := flat[key]; exists {
				return nil, ErrInvalid
			}
			flat[key] = value
		}
	}
	switch operation {
	case "start", "validate", "preview", "cancel", "evidence", "active_profile", "history":
		if len(flat) != 0 {
			return nil, ErrInvalid
		}
	case "capture":
		if len(flat) != 1 || flat["anchor"] != "reference" {
			return nil, ErrInvalid
		}
	case "commit", "rollback":
		if len(flat) != 2 {
			return nil, ErrInvalid
		}
		version, ok := flat["expected_profile_version"].(string)
		n, err := strconv.ParseUint(version, 10, 32)
		hash, hashOK := flat["expected_profile_hash"].(string)
		if !ok || err != nil || n == 0 || strconv.FormatUint(n, 10) != version || !hashOK || !validSHA256(hash) {
			return nil, ErrInvalid
		}
	case "criteria", "set_criteria":
		if _, normalized := flat["response_mode"]; normalized {
			return mq135NormalizedCriteriaPayload(flat)
		}
		names := bytes.Fields([]byte(mq135CriteriaFields))
		if len(flat) != len(names) {
			return nil, ErrInvalid
		}
		for i, name := range names {
			value, exists := flat[string(name)]
			if !exists {
				return nil, ErrInvalid
			}
			if i >= 15 {
				if !mq135Label(value) {
					return nil, ErrInvalid
				}
				continue
			}
			number, ok := value.(json.Number)
			if !ok {
				return nil, ErrInvalid
			}
			n, err := number.Int64()
			if err != nil {
				return nil, ErrInvalid
			}
			switch i {
			case 0:
				if n < 3 || n > 64 {
					return nil, ErrInvalid
				}
			case 1, 2:
				if n <= 0 || n > math.MaxInt64/1000 {
					return nil, ErrInvalid
				}
			case 3, 4, 5, 6, 7:
				if n <= 0 || n > math.MaxUint32 {
					return nil, ErrInvalid
				}
			case 8:
				if n != 1 && n != 2 {
					return nil, ErrInvalid
				}
			case 9, 10, 11:
				if n != 1 {
					return nil, ErrInvalid
				}
			case 12:
				if n < math.MinInt32 || n > math.MaxInt32 {
					return nil, ErrInvalid
				}
			case 13:
				if n < 0 || n > 10000 {
					return nil, ErrInvalid
				}
			case 14:
				if n <= 0 {
					return nil, ErrInvalid
				}
			}
		}
	default:
		return nil, ErrInvalid
	}
	return json.Marshal(flat)
}

func mq135NormalizedCriteriaPayload(flat map[string]any) ([]byte, error) {
	required := [...]string{
		"response_mode", "samples", "min_duration_ms", "max_age_ms",
		"max_variation_milli_percent", "circuit_mv", "divider_top_ohm",
		"divider_bottom_ohm", "topology", "hardware_confirmed",
		"reference_confirmed", "hardware_identity", "reference_environment",
		"reference_kind", "preparation_confirmed", "preparation_method",
		"heating_duration_us",
	}
	allowed := map[string]bool{
		"response_mode": true, "samples": true, "min_duration_ms": true,
		"max_age_ms": true, "max_variation_milli_percent": true,
		"circuit_mv": true, "divider_top_ohm": true, "divider_bottom_ohm": true,
		"topology": true, "hardware_confirmed": true, "reference_confirmed": true,
		"hardware_identity": true, "reference_environment": true, "reference_kind": true,
		"preparation_confirmed": true, "preparation_method": true, "heating_duration_us": true,
		"reference_temperature_centi_c": true, "reference_humidity_centi_pct": true,
	}
	if len(flat) < len(required) || len(flat) > len(required)+2 {
		return nil, ErrInvalid
	}
	for _, key := range required {
		if _, exists := flat[key]; !exists {
			return nil, ErrInvalid
		}
	}
	for key, value := range flat {
		if !allowed[key] {
			return nil, ErrInvalid
		}
		switch key {
		case "response_mode":
			if value != "normalized" {
				return nil, ErrInvalid
			}
		case "hardware_identity", "reference_environment":
			if !mq135Label(value) {
				return nil, ErrInvalid
			}
		case "reference_kind":
			if value != "operational" && value != "clean_air_confirmed" {
				return nil, ErrInvalid
			}
		case "preparation_method":
			if value != "unknown" && value != "operator_declared" {
				return nil, ErrInvalid
			}
		default:
			number, ok := value.(json.Number)
			if !ok {
				return nil, ErrInvalid
			}
			n, err := number.Int64()
			if err != nil {
				return nil, ErrInvalid
			}
			valid := false
			switch key {
			case "samples":
				valid = n >= 3 && n <= 64
			case "min_duration_ms", "max_age_ms":
				valid = n > 0 && n <= math.MaxInt64/1000
			case "max_variation_milli_percent":
				valid = n > 0 && n <= 100000
			case "circuit_mv", "divider_top_ohm", "divider_bottom_ohm":
				valid = n > 0 && uint64(n) <= math.MaxUint32
			case "topology":
				valid = n == 1 || n == 2
			case "hardware_confirmed", "reference_confirmed", "preparation_confirmed":
				valid = n == 0 || n == 1
			case "reference_temperature_centi_c":
				valid = n >= math.MinInt32 && n <= math.MaxInt32
			case "reference_humidity_centi_pct":
				valid = n >= 0 && n <= 10000
			case "heating_duration_us":
				valid = n >= 0
			}
			if !valid {
				return nil, ErrInvalid
			}
		}
	}
	return json.Marshal(flat)
}
