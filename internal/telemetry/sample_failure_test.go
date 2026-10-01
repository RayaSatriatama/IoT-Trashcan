package telemetry

import (
	"encoding/json"
	"testing"
)

func failureSampleBody(t *testing.T, sensor, stream, quality, reason string, raw, converted any) []byte {
	t.Helper()
	record := map[string]any{
		"record_id":    "SIGAP-01/boot-failure/1",
		"record_hash":  "",
		"kind":         "sample",
		"device_id":    "SIGAP-01",
		"boot_id":      "boot-failure",
		"sequence":     "1",
		"monotonic_us": "100",
		"recorded_at":  nil,
		"time_quality": "UNSYNCED",
		"config_ref": map[string]any{
			"schema_version": 1,
			"config_version": "1",
			"config_hash":    "sha256:" + repeatedHex('0'),
		},
		"profile_refs": []any{},
		"sensor_id":    sensor,
		"stream":       stream,
		"quality":      quality,
		"reason":       reason,
		"raw":          raw,
		"converted":    converted,
	}
	record["record_hash"] = "sha256:" + repeatedHex('0')
	withoutHash, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	hash, _, err := RecordHash(withoutHash)
	if err != nil {
		t.Fatal(err)
	}
	record["record_hash"] = hash
	recordBytes, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	body := map[string]any{
		"schema_version": 3,
		"device_id":      "SIGAP-01",
		"batch_id":       "SIGAP-01/boot-failure/sample",
		"record_count":   1,
		"samples":        []json.RawMessage{recordBytes},
		"results":        []any{},
		"events":         []any{},
		"metadata":       []any{},
	}
	bodyBytes, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	return bodyBytes
}

func repeatedHex(value byte) string {
	const hex = "0123456789abcdef"
	result := make([]byte, 64)
	for index := range result {
		result[index] = hex[value&0x0f]
	}
	return string(result)
}

func TestFailedSamplesAllowNullRawWithoutConvertedValue(t *testing.T) {
	for _, test := range []struct {
		name   string
		sensor string
		stream string
		reason string
	}{
		{name: "hx711 timeout", sensor: "hx711_organic", stream: "organic", reason: "NOT_READY"},
		{name: "ultrasonic timeout", sensor: "ultrasonic_b3", stream: "b3", reason: "TIMEOUT"},
		{name: "mq disconnected", sensor: "mq4", stream: "organic", reason: "DISCONNECTED"},
	} {
		t.Run(test.name, func(t *testing.T) {
			if _, err := ValidateBatch(failureSampleBody(t, test.sensor, test.stream, map[string]string{
				"NOT_READY":    "TIMEOUT",
				"TIMEOUT":      "TIMEOUT",
				"DISCONNECTED": "DISCONNECTED",
			}[test.reason], test.reason, nil, nil)); err != nil {
				t.Fatalf("failed sample with no raw evidence rejected: %v", err)
			}
		})
	}
}

func TestReadySampleCannotUseNullRaw(t *testing.T) {
	body := failureSampleBody(t, "mq4", "organic", "READY", "DATA_READY", nil, nil)
	if code := batchErrorCode(t, body); code != MalformedJSON {
		t.Fatalf("READY/null raw error code = %s, want %s", code, MalformedJSON)
	}
}

func TestAcquisitionFailureCannotCarryConvertedValue(t *testing.T) {
	body := failureSampleBody(t, "ultrasonic_b3", "b3", "TIMEOUT", "TIMEOUT", nil, map[string]any{"distance_mm": 100, "fill_percent": 50})
	if code := batchErrorCode(t, body); code != MalformedJSON {
		t.Fatalf("failure converted value error code = %s, want %s", code, MalformedJSON)
	}
}

func TestCrcFailureRetainsCompleteRawEvidence(t *testing.T) {
	raw := map[string]any{
		"temperature_ticks":     30000,
		"humidity_ticks":        40000,
		"temperature_crc_valid": false,
		"humidity_crc_valid":    true,
	}
	body := failureSampleBody(t, "sht31_44", "organic", "CRC_ERROR", "CRC_ERROR", raw, nil)
	if _, err := ValidateBatch(body); err != nil {
		t.Fatalf("CRC failure with complete raw evidence rejected: %v", err)
	}
}
