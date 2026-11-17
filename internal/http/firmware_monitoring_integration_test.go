package httpapi

import (
	"encoding/json"
	"net/http"
	"os"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

func newMonitoringFixture(t *testing.T) *originFixture {
	t.Helper()
	f := newOriginFixture(t)
	query, err := telemetryarchive.NewArchiveQuery(f.db)
	if err != nil {
		t.Fatal(err)
	}
	auth := devicecontrol.NewDatabaseAuthorizer(f.db).WithClock(func() time.Time { return f.now })
	store := devicecontrol.NewStore(f.db).WithClock(func() time.Time { return f.now })
	f.router = NewRouter(nil, WithReadDatabase(f.db, WithReadClock(func() time.Time { return f.now })), WithTelemetryArchive(telemetryarchive.NewService(f.db), query), WithDeviceControl(store, devicecontrol.HTTPOptions{Operator: auth, Device: auth, Diagnostics: store}))
	return f
}

func monitoringRead(t *testing.T, f *originFixture, path, id string) []map[string]json.RawMessage {
	t.Helper()
	w := f.request(http.MethodGet, path, "", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("read status=%d", w.Code)
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
		for _, item := range devices {
			if string(item["id"]) == `"`+id+`"` {
				root = item
				break
			}
		}
		if root == nil {
			t.Fatal("device missing from public directory")
		}
	}
	var samples []map[string]json.RawMessage
	if string(root["firmware_monitoring"]) == "null" {
		t.Fatal("expected a list, not null")
	}
	if err := json.Unmarshal(root["firmware_monitoring"], &samples); err != nil {
		t.Fatal(err)
	}
	return samples
}

func TestFirmwareResultsProductionIngestProjectionAllowlistAndOrigin(t *testing.T) {
	f := newMonitoringFixture(t)
	raw, err := os.ReadFile("../../contracts/telemetry/v3/fixtures/valid/result-fuzzy-unconfigured.json")
	if err != nil {
		t.Fatal(err)
	}
	var batch map[string]any
	if err := json.Unmarshal(raw, &batch); err != nil {
		t.Fatal(err)
	}
	results := batch["results"].([]any)
	record := results[0].(map[string]any)
	record["boot_id"] = "boot-monitor"
	record["sequence"] = "9007199254740993"
	record["record_id"] = "SIGAP-01/boot-monitor/9007199254740993"
	record["source_sample_ids"] = []any{"SIGAP-01/boot-monitor/7"}
	record["monotonic_us"] = "456"
	record["recorded_at"] = nil
	record["time_quality"] = "UNSYNCED"
	f.ingest(t, "SIGAP-01", "result-projection", record)
	got := f.request(http.MethodGet, "/api/devices/origin-id", "", nil)
	if got.Code != http.StatusOK {
		t.Fatalf("GET: %d %s", got.Code, got.Body)
	}
	var response map[string]json.RawMessage
	if err := json.Unmarshal(got.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	var resultsOut []map[string]json.RawMessage
	if err := json.Unmarshal(response["firmware_results"], &resultsOut); err != nil {
		t.Fatal(err)
	}
	if len(resultsOut) != 1 {
		t.Fatalf("results %s", response["firmware_results"])
	}
	item := resultsOut[0]
	if string(item["result_origin"]) != `"UNKNOWN"` || string(item["fresh"]) != "null" || string(item["age_ms"]) != "null" {
		t.Fatalf("origin/time fields: %s", item)
	}
	for _, key := range []string{"config_ref", "profile_refs", "record_hash", "device_id", "computed_monotonic_us"} {
		if _, ok := item[key]; ok {
			t.Fatalf("private field %s leaked", key)
		}
	}
}

func TestMonitoringRoutesKeepOriginZeroAndOwnerReadOnly(t *testing.T) {
	for _, tc := range []struct{ name, tag, origin string }{
		{"physical-v1", "sigap-task6-physical4-v1", "PHYSICAL"},
		{"physical-v2", "sigap-task6-physical4-v2", "PHYSICAL"},
		{"simulation", "sigap-task6-sensor-sim-v1", "SIMULATED"},
		{"spoofed-prefix", "sigap-task6-physical4-v2-extra", "UNKNOWN"},
		{"missing-origin", "", "UNKNOWN"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := newMonitoringFixture(t)
			sample := originSample(t, "SIGAP-01", "boot-monitor", "9007199254740993", false)
			sample["sensor_id"] = "mq135"
			if tc.tag != "" {
				f.ingest(t, "SIGAP-01", "boot", originMetadata(t, "SIGAP-01", "boot-monitor", 1, tc.tag))
			}
			f.ingest(t, "SIGAP-01", "sample", sample)
			f.db.SetMaxOpenConns(1)
			if _, err := f.db.Exec("PRAGMA query_only=ON"); err != nil {
				t.Fatal(err)
			}
			for _, path := range []string{"/api/devices/origin-id", "/api/devices", "/api/locations/origin-loc"} {
				items := monitoringRead(t, f, path, "origin-id")
				if len(items) != 1 {
					t.Fatalf("%s: samples=%d", path, len(items))
				}
				got := items[0]
				if string(got["sample_origin"]) != `"`+tc.origin+`"` || string(got["raw"]) != `{"adc_counts":0}` || string(got["last_sequence"]) != `"9007199254740993"` {
					t.Fatalf("origin/raw/sequence mismatch: %s", path)
				}
				if string(got["age_ms"]) != "null" || string(got["fresh"]) != "null" {
					t.Fatal("unknown time was promoted to fresh")
				}
				allowed := map[string]bool{"sensor_id": true, "stream": true, "boot_id": true, "quality": true, "reason": true, "sample_origin": true, "origin_firmware_version": true, "last_sequence": true, "raw": true, "converted_value": true, "fresh": true, "age_ms": true, "ordering_unknown": true, "future_clock_skew": true}
				if len(got) != len(allowed) {
					t.Fatal("monitoring DTO contract changed")
				}
				for key := range got {
					if !allowed[key] {
						t.Fatalf("non-allowlisted monitoring field %s", key)
					}
				}
			}
			if items := monitoringRead(t, f, "/api/devices/origin-other", "origin-other"); len(items) != 0 {
				t.Fatal("sample crossed device ownership")
			}
		})
	}
}

func TestMonitoringRoutesKeepClockAndQualityUncertainty(t *testing.T) {
	for _, future := range []bool{false, true} {
		f := newMonitoringFixture(t)
		sample := originSample(t, "SIGAP-01", "boot-source", "1", true)
		if future {
			sample["recorded_at"] = f.now.Add(time.Second).Format(time.RFC3339Nano)
		} else {
			sample["quality"], sample["recorded_at"] = "STALE", f.now.Format(time.RFC3339Nano)
		}
		f.ingest(t, "SIGAP-01", "timing", sample)
		got := monitoringRead(t, f, "/api/devices/origin-id", "origin-id")[0]
		if future {
			if string(got["future_clock_skew"]) != "true" || string(got["age_ms"]) != "null" || string(got["fresh"]) != "null" {
				t.Fatal("future clock was clamped to zero/fresh")
			}
		} else if string(got["age_ms"]) != "0" || string(got["fresh"]) != "false" || string(got["quality"]) != `"STALE"` {
			t.Fatal("explicit stale quality/zero age was lost")
		}
	}
	f := newMonitoringFixture(t)
	f.ingest(t, "SIGAP-01", "a", originSample(t, "SIGAP-01", "boot-a", "90", false))
	f.ingest(t, "SIGAP-01", "b", originSample(t, "SIGAP-01", "boot-b", "3", false))
	got := monitoringRead(t, f, "/api/devices/origin-id", "origin-id")[0]
	if string(got["ordering_unknown"]) != "true" || string(got["boot_id"]) != `"boot-a"` {
		t.Fatal("cross-boot uncertainty was lost")
	}
}

func TestMonitoringNumericAllowlistKeepsZeroAndDropsPrivateFields(t *testing.T) {
	got := monitoringNumbers(json.RawMessage(`{"counts":0,"rom":"private-test","calibration_profile":{"factor":2},"adc_counts":null}`), monitoringRawKeys)
	if len(got) != 1 || got["counts"] != 0 {
		t.Fatal("numeric allowlist changed")
	}
	for _, raw := range []string{`null`, `{}`, `{"counts":"3"}`, `{"counts":null}`, `invalid`} {
		if monitoringNumbers(json.RawMessage(raw), monitoringRawKeys) != nil {
			t.Fatal("absent/invalid numeric value was invented")
		}
	}
}

func TestMQ135AdditiveFieldsAreIngestedAndProjected(t *testing.T) {
	f := newMonitoringFixture(t)
	invalid := originSample(t, "SIGAP-01", "boot-mq135", "3", false)
	invalid["sensor_id"], invalid["quality"], invalid["reason"] = "mq135", "READY", "DATA_READY"
	invalid["converted"] = map[string]any{"adc_mv": 2500, "adc_pin_mv": 1100, "sensor_ao_mv": 2500, "adc_efuse_calibrated": 0}
	bad := f.request(http.MethodPost, "/api/telemetry/batches", "origin-test-device", originBody(t, "SIGAP-01", "bad-mq135-marker", invalid))
	if bad.Code != http.StatusBadRequest {
		t.Fatalf("false calibration marker accepted: %d %s", bad.Code, bad.Body)
	}
	var stored int
	if err := f.db.QueryRow(`SELECT count(*) FROM telemetry_records`).Scan(&stored); err != nil || stored != 0 {
		t.Fatalf("invalid calibrated sample mutated archive: count=%d err=%v", stored, err)
	}

	legacy := originSample(t, "SIGAP-01", "boot-mq135", "1", false)
	legacy["sensor_id"], legacy["quality"], legacy["reason"] = "mq135", "READY", "DATA_READY"
	legacy["converted"] = map[string]any{"adc_mv": 1650}
	calibrated := originSample(t, "SIGAP-01", "boot-mq135", "2", false)
	calibrated["sensor_id"], calibrated["quality"], calibrated["reason"] = "mq135", "READY", "DATA_READY"
	calibrated["converted"] = map[string]any{"adc_mv": 2500, "adc_pin_mv": 1100, "sensor_ao_mv": 2500, "adc_efuse_calibrated": 1}
	f.ingest(t, "SIGAP-01", "mq135-legacy", legacy)
	legacyMonitoring := monitoringRead(t, f, "/api/devices/origin-id", "origin-id")
	if len(legacyMonitoring) != 1 {
		t.Fatalf("legacy monitoring samples=%d", len(legacyMonitoring))
	}
	var legacyConverted map[string]json.RawMessage
	if err := json.Unmarshal(legacyMonitoring[0]["converted_value"], &legacyConverted); err != nil {
		t.Fatal(err)
	}
	if len(legacyConverted) != 1 || string(legacyConverted["adc_mv"]) != "1650" {
		t.Fatalf("legacy monitoring value changed: %s", legacyMonitoring[0]["converted_value"])
	}
	f.ingest(t, "SIGAP-01", "mq135-calibrated", calibrated)

	monitoring := monitoringRead(t, f, "/api/devices/origin-id", "origin-id")
	if len(monitoring) != 1 {
		t.Fatalf("monitoring samples=%d", len(monitoring))
	}
	var live struct {
		Converted map[string]json.RawMessage `json:"converted_value"`
	}
	if err := json.Unmarshal(monitoring[0]["converted_value"], &live.Converted); err != nil {
		t.Fatal(err)
	}
	if len(live.Converted) != 4 || string(live.Converted["adc_mv"]) != "2500" || string(live.Converted["sensor_ao_mv"]) != "2500" || string(live.Converted["adc_pin_mv"]) != "1100" || string(live.Converted["adc_efuse_calibrated"]) != "1" {
		t.Fatalf("MQ135 monitoring fields: %s", monitoring[0]["converted_value"])
	}

	history := f.request(http.MethodGet, "/api/devices/origin-id/sensor-history?sensor_id=mq135&boot_id=boot-mq135", "", nil)
	if history.Code != http.StatusOK {
		t.Fatalf("history status=%d body=%s", history.Code, history.Body)
	}
	var page struct {
		Points []struct {
			Sequence  string                     `json:"sequence"`
			Converted map[string]json.RawMessage `json:"converted"`
		} `json:"points"`
	}
	if err := json.Unmarshal(history.Body.Bytes(), &page); err != nil {
		t.Fatal(err)
	}
	if len(page.Points) != 2 {
		t.Fatalf("history points=%d body=%s", len(page.Points), history.Body)
	}
	for _, point := range page.Points {
		switch point.Sequence {
		case "1":
			if len(point.Converted) != 1 || string(point.Converted["adc_mv"]) != "1650" {
				t.Fatalf("legacy history was rewritten: %#v", point.Converted)
			}
		case "2":
			if len(point.Converted) != 4 || string(point.Converted["adc_pin_mv"]) != "1100" || string(point.Converted["sensor_ao_mv"]) != "2500" || string(point.Converted["adc_efuse_calibrated"]) != "1" {
				t.Fatalf("calibrated history fields: %#v", point.Converted)
			}
		default:
			t.Fatalf("unexpected history sequence %q", point.Sequence)
		}
	}
}

func TestDeviceReadExposesUncalibratedMQ135ZeroFromArchive(t *testing.T) {
	f := newOriginFixture(t)
	query, err := telemetryarchive.NewArchiveQuery(f.db)
	if err != nil {
		t.Fatal(err)
	}
	auth := devicecontrol.NewDatabaseAuthorizer(f.db).WithClock(func() time.Time { return f.now })
	store := devicecontrol.NewStore(f.db).WithClock(func() time.Time { return f.now })
	f.router = NewRouter(nil, WithReadDatabase(f.db, WithReadClock(func() time.Time { return f.now })), WithTelemetryArchive(telemetryarchive.NewService(f.db), query), WithDeviceControl(store, devicecontrol.HTTPOptions{Operator: auth, Device: auth, Diagnostics: store}))
	sample := originSample(t, "SIGAP-01", "boot-monitoring", "1", false)
	sample["sensor_id"] = "mq135"
	sample["quality"], sample["reason"] = "UNCALIBRATED", "PROFILE_MISSING"
	f.ingest(t, "SIGAP-01", "mq135-zero", sample)

	w := f.request(http.MethodGet, "/api/devices/origin-id", "", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("device read status=%d body=%s", w.Code, w.Body)
	}
	var response map[string]json.RawMessage
	if err := json.Unmarshal(w.Body.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	var slots []json.RawMessage
	if err := json.Unmarshal(response["slots"], &slots); err != nil {
		t.Fatal(err)
	}
	if len(slots) != 0 {
		t.Fatalf("legacy readings unexpectedly populated: %s", response["slots"])
	}
	var samples []map[string]json.RawMessage
	if err := json.Unmarshal(response["firmware_monitoring"], &samples); err != nil {
		t.Fatalf("missing or invalid firmware monitoring projection: %s", response["firmware_monitoring"])
	}
	if len(samples) != 1 {
		t.Fatalf("expected one archived sensor sample, got %d: %s", len(samples), response["firmware_monitoring"])
	}
	sampleJSON, _ := json.Marshal(samples[0])
	var got struct {
		SensorID  string                     `json:"sensor_id"`
		Quality   string                     `json:"quality"`
		Reason    string                     `json:"reason"`
		Raw       map[string]json.RawMessage `json:"raw"`
		Converted json.RawMessage            `json:"converted_value"`
	}
	if err := json.Unmarshal(sampleJSON, &got); err != nil {
		t.Fatal(err)
	}
	if got.SensorID != "mq135" || got.Quality != "UNCALIBRATED" || got.Reason != "PROFILE_MISSING" {
		t.Fatalf("quality projection: %s", sampleJSON)
	}
	if string(got.Raw["adc_counts"]) != "0" {
		t.Fatalf("raw present-zero lost: %s", sampleJSON)
	}
	if string(got.Converted) != "null" {
		t.Fatalf("uncalibrated conversion must remain null: %s", sampleJSON)
	}
	for _, forbidden := range []string{"rom", "token", "credential", "calibration_profile", "readings"} {
		if _, ok := samples[0][forbidden]; ok {
			t.Fatalf("protected/unrelated field %q exposed: %s", forbidden, sampleJSON)
		}
	}
}
