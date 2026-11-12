package httpapi

import (
	"encoding/json"
	"net/http"
	"os"
	"strings"
	"testing"
)

func contractResult(t *testing.T, boot, seq, quality string, recorded any) map[string]any {
	t.Helper()
	raw, err := os.ReadFile("../../contracts/telemetry/v3/fixtures/valid/result-fuzzy-unconfigured.json")
	if err != nil {
		t.Fatal(err)
	}
	var batch map[string]any
	if err := json.Unmarshal(raw, &batch); err != nil {
		t.Fatal(err)
	}
	r := batch["results"].([]any)[0].(map[string]any)
	r["boot_id"], r["sequence"] = boot, seq
	r["record_id"] = "SIGAP-01/" + boot + "/" + seq
	r["source_sample_ids"] = []any{"SIGAP-01/" + boot + "/7"}
	r["time_quality"], r["recorded_at"] = quality, recorded
	return r
}

func resultRead(t *testing.T, f *originFixture, path string) []map[string]json.RawMessage {
	t.Helper()
	w := f.request(http.MethodGet, path, "", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("GET %s: %d %s", path, w.Code, w.Body)
	}
	var root map[string]json.RawMessage
	if err := json.Unmarshal(w.Body.Bytes(), &root); err != nil {
		t.Fatal(err)
	}
	if root["devices"] != nil {
		var devices []map[string]json.RawMessage
		if err := json.Unmarshal(root["devices"], &devices); err != nil {
			t.Fatal(err)
		}
		root = nil
		for _, d := range devices {
			if string(d["id"]) == `"origin-id"` {
				root = d
				break
			}
		}
		if root == nil {
			return nil
		}
	}
	if len(root["firmware_results"]) == 0 || string(root["firmware_results"]) == "null" {
		return nil
	}
	var out []map[string]json.RawMessage
	if err := json.Unmarshal(root["firmware_results"], &out); err != nil {
		t.Fatalf("results at %s: %s: %v", path, root["firmware_results"], err)
	}
	return out
}

func TestFirmwareResultOriginRequiresMatchingBootAndExactPhysicalTag(t *testing.T) {
	for _, tc := range []struct {
		tag             any
		origin, version string
	}{{"sigap-task6-physical4-v2", "PHYSICAL", `"sigap-task6-physical4-v2"`}, {"sigap-task6-sensor-sim-v1", "SIMULATED", `"sigap-task6-sensor-sim-v1"`}, {nil, "UNKNOWN", "null"}, {"sigap-task6-physical4-v2-extra", "UNKNOWN", "null"}, {"sigap-task6-physical4-v2", "UNKNOWN", "null"}} {
		f := newMonitoringFixture(t)
		if tc.tag != nil {
			f.ingest(t, "SIGAP-01", "boot", originMetadata(t, "SIGAP-01", "b", 1, tc.tag))
		}
		// Conflicting metadata must not make attribution falsely physical.
		if tc.origin == "UNKNOWN" && tc.tag != nil {
			f.ingest(t, "SIGAP-01", "other", originMetadata(t, "SIGAP-01", "b", 2, "sigap-task6-sensor-sim-v1"))
		}
		f.ingest(t, "SIGAP-01", "result", contractResult(t, "b", "9", "UNSYNCED", nil))
		got := resultRead(t, f, "/api/devices/origin-id")[0]
		if string(got["result_origin"]) != `"`+tc.origin+`"` || string(got["origin_firmware_version"]) != tc.version {
			t.Fatalf("origin/version: %s", got)
		}
		if string(got["fresh"]) != "null" || string(got["age_ms"]) != "null" {
			t.Fatalf("UNSYNCED result aged: %s", got)
		}
	}
}

func TestFirmwareResultWrongBootMetadataCannotAttribute(t *testing.T) {
	f := newMonitoringFixture(t)
	f.ingest(t, "SIGAP-01", "metadata", originMetadata(t, "SIGAP-01", "boot-B", 1, "sigap-task6-physical4-v2"))
	f.ingest(t, "SIGAP-01", "result", contractResult(t, "boot-A", "9", "UNSYNCED", nil))
	got := resultRead(t, f, "/api/devices/origin-id")[0]
	if string(got["result_origin"]) != `"UNKNOWN"` || string(got["origin_firmware_version"]) != "null" {
		t.Fatalf("wrong-boot metadata attributed: %s", got)
	}
}

func TestFirmwareResultsEmptyArrayOnOwnerReads(t *testing.T) {
	f := newMonitoringFixture(t)
	for _, path := range []string{"/api/devices/origin-id", "/api/devices", "/api/locations/origin-loc"} {
		t.Run(path, func(t *testing.T) {
			w := f.request(http.MethodGet, path, "", nil)
			if w.Code != http.StatusOK {
				t.Fatalf("GET %s: %d %s", path, w.Code, w.Body)
			}
			var value any
			if err := json.Unmarshal(w.Body.Bytes(), &value); err != nil {
				t.Fatal(err)
			}
			var findOwner func(any) map[string]json.RawMessage
			findOwner = func(v any) map[string]json.RawMessage {
				switch x := v.(type) {
				case map[string]any:
					if x["id"] == "origin-id" {
						raw, _ := json.Marshal(x)
						var obj map[string]json.RawMessage
						_ = json.Unmarshal(raw, &obj)
						return obj
					}
					for _, child := range x {
						if got := findOwner(child); got != nil {
							return got
						}
					}
				case []any:
					for _, child := range x {
						if got := findOwner(child); got != nil {
							return got
						}
					}
				}
				return nil
			}
			owner := findOwner(value)
			if owner == nil {
				t.Fatalf("owner not found in %s: %s", path, w.Body)
			}
			results, exists := owner["firmware_results"]
			if !exists || string(results) != "[]" {
				t.Fatalf("firmware_results = %s (present=%v), want []", results, exists)
			}
		})
	}
}

func TestFirmwareResultOwnerRoutesAndReadOnlyQuery(t *testing.T) {
	f := newMonitoringFixture(t)
	f.ingest(t, "SIGAP-01", "result", contractResult(t, "b", "9", "UNSYNCED", nil))
	if _, err := f.db.Exec("PRAGMA query_only=ON"); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{"/api/devices/origin-id", "/api/devices", "/api/locations/origin-loc"} {
		got := resultRead(t, f, path)
		if len(got) != 1 {
			t.Fatalf("%s results=%d", path, len(got))
		}
	}
	if got := resultRead(t, f, "/api/devices/origin-other"); len(got) != 0 {
		t.Fatalf("cross-owner result leak: %d", len(got))
	}
	w := f.request(http.MethodGet, "/api/devices/origin-id", "", nil)
	var root map[string]json.RawMessage
	if err := json.Unmarshal(w.Body.Bytes(), &root); err != nil {
		t.Fatal(err)
	}
	var slots []json.RawMessage
	if err := json.Unmarshal(root["slots"], &slots); err != nil || len(slots) != 0 {
		t.Fatalf("legacy slots changed: %s", root["slots"])
	}
}

func TestFirmwareResultClockProjection(t *testing.T) {
	for _, tc := range []struct {
		name, quality      string
		at                 any
		age, fresh, future string
	}{{"future", "SYNCED", "2026-10-29T05:00:01Z", "null", "null", "true"}, {"now", "SYNCED", "2026-10-29T05:00:00Z", "0", "null", "false"}, {"unsynced", "UNSYNCED", nil, "null", "null", "false"}, {"unsynced-with-time", "UNSYNCED", "2026-10-29T05:00:00Z", "null", "null", "false"}} {
		t.Run(tc.name, func(t *testing.T) {
			f := newMonitoringFixture(t)
			f.ingest(t, "SIGAP-01", "result", contractResult(t, "b", "9", tc.quality, tc.at))
			got := resultRead(t, f, "/api/devices/origin-id")[0]
			if string(got["age_ms"]) != tc.age || string(got["fresh"]) != tc.fresh || string(got["future_clock_skew"]) != tc.future {
				t.Fatalf("clock projection: %s", got)
			}
		})
	}
}

func TestFirmwareResultCrossBootOrderingAndUint64Precision(t *testing.T) {
	f := newMonitoringFixture(t)
	a := contractResult(t, "boot-A", "9007199254740993", "UNSYNCED", nil)
	a["processed"].(map[string]any)["fill_pct"] = 0
	f.ingest(t, "SIGAP-01", "a", a)
	f.ingest(t, "SIGAP-01", "b", contractResult(t, "boot-B", "3", "UNSYNCED", nil))
	got := resultRead(t, f, "/api/devices/origin-id")
	if len(got) != 1 || string(got[0]["boot_id"]) != `"boot-A"` || string(got[0]["sequence"]) != `"9007199254740993"` || string(got[0]["ordering_unknown"]) != "true" {
		t.Fatalf("cross-boot order/precision: %s", got)
	}
	var processed map[string]json.RawMessage
	if err := json.Unmarshal(got[0]["processed"], &processed); err != nil || string(processed["fill_pct"]) != "0" || string(processed["weight_kg"]) != "null" {
		t.Fatalf("zero/null processed values: %s", got[0]["processed"])
	}
}

func TestFirmwareResultNestedDTOAllowlistStripsSecrets(t *testing.T) {
	raw := []byte(`{"stream":"organic","boot_id":"b","sequence":"9","monotonic_us":"456","recorded_at":null,"time_quality":"UNSYNCED","source_sample_ids":[],"processed":{"accumulation_hours":null,"fill_pct":0,"ogi":null,"temperature_c":null,"weight_kg":null,"weight_utilization_pct":null,"private_profile":"nested-secret"},"fuzzy":{"label":null,"model_state":"UNCONFIGURED","reason":"FIS_NOT_CONFIGURED","score":null,"state":"UNCONFIGURED","trace":{"secret":"fuzzy-secret"}},"safety":{"model_state":"UNCONFIGURED","reason":"SAFETY_NOT_CONFIGURED","state":"UNAVAILABLE","trace":{"token":"safety-secret"}},"trace":{"credentials":"trace-secret"},"result_origin":"UNKNOWN","origin_firmware_version":null,"fresh":null,"age_ms":null,"future_clock_skew":false}`)
	var dto firmwareResultDTO
	if err := json.Unmarshal(raw, &dto); err != nil {
		t.Fatal(err)
	}
	out, err := json.Marshal(dto)
	if err != nil {
		t.Fatal(err)
	}
	for _, secret := range []string{"nested-secret", "fuzzy-secret", "safety-secret", "trace-secret"} {
		if strings.Contains(string(out), secret) {
			t.Fatalf("nested secret leaked: %s", out)
		}
	}
	var got map[string]json.RawMessage
	if err := json.Unmarshal(out, &got); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"sequence", "monotonic_us", "result_origin", "origin_firmware_version", "fresh", "age_ms", "future_clock_skew"} {
		if _, ok := got[key]; !ok {
			t.Fatalf("allowlisted key %s omitted: %s", key, out)
		}
	}
	var processed map[string]json.RawMessage
	if err := json.Unmarshal(got["processed"], &processed); err != nil || string(processed["fill_pct"]) != "0" || string(processed["weight_kg"]) != "null" {
		t.Fatalf("processed zero/null lost: %s", got["processed"])
	}
}
