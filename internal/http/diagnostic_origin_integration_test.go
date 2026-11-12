package httpapi

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

// These are TEST-only contract fixtures, not firmware producer evidence.
type originFixture struct {
	db     *sql.DB
	router http.Handler
	now    time.Time
}

func newOriginFixture(t *testing.T) *originFixture {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "origin.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	sum := sha256.Sum256([]byte("origin-test-device"))
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES('origin-loc','TEST'); INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES('origin-id','origin-loc','SIGAP-01',?),('origin-other','origin-loc','SIGAP-02',?)`, hex.EncodeToString(sum[:]), hex.EncodeToString(sum[:])); err != nil {
		t.Fatal(err)
	}
	f := &originFixture{db: db, now: time.Date(2026, 10, 29, 5, 0, 0, 0, time.UTC)}
	if err := devicecontrol.ProvisionOperatorSession(t.Context(), db, "origin-session", "origin-operator", "origin-test-operator", "origin-test-csrf", f.now.Add(time.Hour), []string{"SIGAP-01"}); err != nil {
		t.Fatal(err)
	}
	auth := devicecontrol.NewDatabaseAuthorizer(db).WithClock(func() time.Time { return f.now })
	store := devicecontrol.NewStore(db).WithClock(func() time.Time { return f.now })
	query, err := telemetryarchive.NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	f.router = NewRouter(nil, WithTelemetryArchive(telemetryarchive.NewService(db), query), WithDeviceControl(store, devicecontrol.HTTPOptions{Operator: auth, Device: auth, Diagnostics: store}))
	return f
}

func (f *originFixture) request(method, path, token string, body []byte) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	r.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	f.router.ServeHTTP(w, r)
	return w
}

func originSample(t *testing.T, device, boot, sequence string, synced bool) map[string]any {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("..", "..", "contracts", "telemetry", "v3", "canonical", "sample-mq4-uncalibrated.json"))
	if err != nil {
		t.Fatal(err)
	}
	var r map[string]any
	if err := json.Unmarshal(raw, &r); err != nil {
		t.Fatal(err)
	}
	r["device_id"], r["boot_id"], r["sequence"] = device, boot, sequence
	r["record_id"] = device + "/" + boot + "/" + sequence
	r["monotonic_us"] = "18446744073709551615"
	r["raw"] = map[string]any{"adc_counts": 0}
	if synced {
		r["time_quality"], r["recorded_at"] = "SYNCED", "2026-10-29T04:59:59Z"
	}
	return r
}

func originMetadata(t *testing.T, device, boot string, sequence int, tag any) map[string]any {
	t.Helper()
	r := originSample(t, device, boot, fmt.Sprint(sequence), false)
	for _, key := range []string{"sensor_id", "stream", "quality", "reason", "raw", "converted"} {
		delete(r, key)
	}
	r["kind"], r["metadata_type"] = "metadata", "BOOT"
	r["payload"] = map[string]any{"firmware_version": tag, "reset_reason": "POWER_ON"}
	if tag == nil {
		r["metadata_type"], r["payload"] = "HEALTH_SNAPSHOT", map[string]any{"entries": []any{}}
	}
	return r
}

func originBody(t *testing.T, device, batch string, records ...map[string]any) []byte {
	t.Helper()
	body := map[string]any{"schema_version": 3, "device_id": device, "batch_id": device + "/" + batch, "record_count": len(records), "samples": []any{}, "metadata": []any{}, "events": []any{}, "results": []any{}}
	for _, r := range records {
		r["record_hash"] = "sha256:" + string(bytes.Repeat([]byte("0"), 64))
		raw, err := json.Marshal(r)
		if err != nil {
			t.Fatal(err)
		}
		hash, _, err := telemetry.RecordHash(raw)
		if err != nil {
			t.Fatal(err)
		}
		r["record_hash"] = hash
		key := "samples"
		if r["kind"] == "metadata" {
			key = "metadata"
		} else if r["kind"] == "result" {
			key = "results"
		}
		body[key] = append(body[key].([]any), r)
	}
	raw, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func (f *originFixture) ingest(t *testing.T, device, batch string, records ...map[string]any) {
	t.Helper()
	body := originBody(t, device, batch, records...)
	w := f.request(http.MethodPost, "/api/telemetry/batches", "origin-test-device", body)
	if w.Code != http.StatusOK {
		t.Fatalf("ingest %d: %s", w.Code, w.Body)
	}
	var ack telemetry.Ack
	if err := json.Unmarshal(w.Body.Bytes(), &ack); err != nil {
		t.Fatal(err)
	}
	if ack.BodyHash != telemetry.BodyHash(body) || ack.Status != "DURABLE" {
		t.Fatalf("invalid ACK: %s", w.Body)
	}
	var saved []byte
	if err := f.db.QueryRow(`SELECT body_bytes FROM telemetry_batches WHERE device_id=? AND batch_id=?`, device, device+"/"+batch).Scan(&saved); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(saved, body) {
		t.Fatal("durable batch bytes changed")
	}
}

func (f *originFixture) diagnostic(t *testing.T) map[string]json.RawMessage {
	t.Helper()
	w := f.request(http.MethodGet, "/api/device-control/commands/SIGAP-01/diagnostics?sensor_id=mq4", "origin-test-operator", nil)
	if w.Code != http.StatusOK {
		t.Fatalf("diagnostic %d: %s", w.Code, w.Body)
	}
	var diagnostic map[string]json.RawMessage
	if err := json.Unmarshal(w.Body.Bytes(), &diagnostic); err != nil {
		t.Fatal(err)
	}
	if diagnostic["sensor_id"] == nil {
		t.Fatalf("missing diagnostic: %s", w.Body)
	}
	return diagnostic
}

func TestDiagnosticOriginDurableAuthenticatedMatrix(t *testing.T) {
	for _, tc := range []struct {
		name string
		tags []any
		want string
	}{
		{"simulated", []any{"sigap-task6-sensor-sim-v1"}, "SIMULATED"},
		{"physical", []any{"sigap-task6"}, "PHYSICAL"},
		{"missing", nil, "UNKNOWN"},
		{"unrecognized", []any{"future-version"}, "UNKNOWN"},
		{"whitespace", []any{" sigap-task6"}, "UNKNOWN"},
		{"conflicting", []any{"sigap-task6", "sigap-task6-sensor-sim-v1"}, "UNKNOWN"},
		{"duplicate", []any{"sigap-task6", "sigap-task6"}, "UNKNOWN"},
	} {
		for _, synced := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/synced=%t", tc.name, synced), func(t *testing.T) {
				f := newOriginFixture(t)
				records := []map[string]any{}
				// More than three unrelated metadata records must not hide BOOT.
				for i := 1; i <= 4; i++ {
					records = append(records, originMetadata(t, "SIGAP-01", "boot-source", i, nil))
				}
				for i, tag := range tc.tags {
					records = append(records, originMetadata(t, "SIGAP-01", "boot-source", i+5, tag))
				}
				records = append(records, originSample(t, "SIGAP-01", "boot-source", "18446744073709551615", synced))
				f.ingest(t, "SIGAP-01", "matrix", records...)
				d := f.diagnostic(t)
				if string(d["sample_origin"]) != fmt.Sprintf("%q", tc.want) || string(d["boot_id"]) != `"boot-source"` {
					t.Fatalf("origin/boot: %s", d)
				}
				if tc.want == "UNKNOWN" && string(d["origin_firmware_version"]) != "null" {
					t.Fatalf("unknown version: %s", d)
				}
				if tc.want != "UNKNOWN" && string(d["origin_firmware_version"]) != fmt.Sprintf("%q", tc.tags[0]) {
					t.Fatalf("version: %s", d)
				}
				if string(d["raw"]) != `{"adc_counts":0}` || string(d["converted_value"]) != "null" || string(d["last_sequence"]) != `"18446744073709551615"` {
					t.Fatalf("zero/null/precision changed: %s", d)
				}
				if !synced && (string(d["fresh"]) != "null" || string(d["age_ms"]) != "null") {
					t.Fatalf("unsynced freshness invented: %s", d)
				}
				if synced && (string(d["fresh"]) != "null" || string(d["age_ms"]) != "1000") {
					t.Fatalf("synced freshness: %s", d)
				}
			})
		}
	}
}

func TestDiagnosticOriginIgnoresOtherDeviceAndBoot(t *testing.T) {
	f := newOriginFixture(t)
	f.ingest(t, "SIGAP-01", "sample", originSample(t, "SIGAP-01", "boot-source", "99", false))
	f.ingest(t, "SIGAP-01", "otherboot", originMetadata(t, "SIGAP-01", "other-boot", 1, "sigap-task6-sensor-sim-v1"))
	f.ingest(t, "SIGAP-02", "otherdevice", originMetadata(t, "SIGAP-02", "boot-source", 1, "sigap-task6-sensor-sim-v1"))
	if d := f.diagnostic(t); string(d["sample_origin"]) != `"UNKNOWN"` {
		t.Fatalf("cross-identity origin: %s", d)
	}
}

func TestDiagnosticOriginConflictBeyondMetadataPrefix(t *testing.T) {
	f := newOriginFixture(t)
	records := []map[string]any{originMetadata(t, "SIGAP-01", "boot-source", 1, "sigap-task6")}
	for i := 2; i <= 5; i++ {
		records = append(records, originMetadata(t, "SIGAP-01", "boot-source", i, nil))
	}
	records = append(records, originMetadata(t, "SIGAP-01", "boot-source", 6, "sigap-task6-sensor-sim-v1"), originSample(t, "SIGAP-01", "boot-source", "99", false))
	f.ingest(t, "SIGAP-01", "conflict", records...)
	if d := f.diagnostic(t); string(d["sample_origin"]) != `"UNKNOWN"` {
		t.Fatalf("hidden conflicting BOOT: %s", d)
	}
}

func TestDiagnosticPreservesCrossBootOrderingUncertainty(t *testing.T) {
	f := newOriginFixture(t)
	f.ingest(t, "SIGAP-01", "boot-a", originSample(t, "SIGAP-01", "boot-a", "90", false))
	f.ingest(t, "SIGAP-01", "boot-b", originSample(t, "SIGAP-01", "boot-b", "3", false))
	d := f.diagnostic(t)
	if string(d["ordering_unknown"]) != `true` {
		t.Fatalf("cross-boot uncertainty lost: %s", d)
	}
	if string(d["boot_id"]) != `"boot-a"` {
		t.Fatalf("selected boot identity changed: %s", d)
	}
}

func TestDiagnosticReportsFutureClockSkewWithoutClampingAge(t *testing.T) {
	f := newOriginFixture(t)
	sample := originSample(t, "SIGAP-01", "boot-source", "1", true)
	sample["recorded_at"] = "2026-10-29T05:00:01Z"
	f.ingest(t, "SIGAP-01", "future-clock", sample)
	d := f.diagnostic(t)
	if string(d["future_clock_skew"]) != "true" || string(d["age_ms"]) != "null" || string(d["fresh"]) != "null" {
		t.Fatalf("future clock was represented as zero-age/fresh: %s", d)
	}
}

func TestDiagnosticKeepsExplicitBadQualityAndZeroAge(t *testing.T) {
	for _, quality := range []string{"STALE"} {
		t.Run(quality, func(t *testing.T) {
			f := newOriginFixture(t)
			sample := originSample(t, "SIGAP-01", "boot-source", "1", true)
			sample["quality"], sample["recorded_at"] = quality, f.now.Format(time.RFC3339Nano)
			f.ingest(t, "SIGAP-01", "quality-"+quality, sample)
			d := f.diagnostic(t)
			if string(d["fresh"]) != "false" || string(d["age_ms"]) != "0" {
				t.Fatalf("quality/valid zero age lost: %s", d)
			}
		})
	}
}

func TestDiagnosticOriginAuthorizationAndInvalidIngest(t *testing.T) {
	f := newOriginFixture(t)
	body := originBody(t, "SIGAP-01", "unauthorized", originSample(t, "SIGAP-01", "boot-source", "99", false))
	for _, token := range []string{"", "wrong", "origin-test-operator"} {
		if w := f.request(http.MethodPost, "/api/telemetry/batches", token, body); w.Code != http.StatusUnauthorized {
			t.Fatalf("unauthorized ingest status %d", w.Code)
		}
	}
	var count int
	if err := f.db.QueryRow(`SELECT count(*) FROM telemetry_records`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("unauthorized mutation count=%d err=%v", count, err)
	}
	f.ingest(t, "SIGAP-01", "authorized", originSample(t, "SIGAP-01", "boot-source", "99", false))
	path := "/api/device-control/commands/SIGAP-01/diagnostics?sensor_id=mq4"
	for _, token := range []string{"", "wrong", "origin-test-device"} {
		if w := f.request(http.MethodGet, path, token, nil); w.Code != http.StatusUnauthorized {
			t.Fatalf("unauthorized diagnostics status %d", w.Code)
		}
	}
	if w := f.request(http.MethodGet, "/api/device-control/commands/SIGAP-02/diagnostics?sensor_id=mq4", "origin-test-operator", nil); w.Code != http.StatusUnauthorized {
		t.Fatalf("permission status %d", w.Code)
	}
	f.now = f.now.Add(time.Hour)
	if w := f.request(http.MethodGet, path, "origin-test-operator", nil); w.Code != http.StatusUnauthorized {
		t.Fatalf("expired operator status %d", w.Code)
	}
	bad := originBody(t, "SIGAP-01", "malformed", originMetadata(t, "SIGAP-01", "boot-source", 1, 123))
	if w := f.request(http.MethodPost, "/api/telemetry/batches", "origin-test-device", bad); w.Code != http.StatusBadRequest {
		t.Fatalf("malformed BOOT status=%d body=%s", w.Code, w.Body)
	}
	if err := f.db.QueryRow(`SELECT count(*) FROM telemetry_records`).Scan(&count); err != nil || count != 1 {
		t.Fatalf("invalid ingest mutated count=%d err=%v", count, err)
	}
}
