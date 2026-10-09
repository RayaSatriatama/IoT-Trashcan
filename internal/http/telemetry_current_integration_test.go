package httpapi

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

func integratedSample(t *testing.T, boot, sequence, sensor string, monotonic string) map[string]any {
	t.Helper()
	record := map[string]any{
		"record_id":    "SIGAP-01/" + boot + "/" + sequence,
		"record_hash":  "sha256:" + strings.Repeat("0", 64),
		"kind":         "sample",
		"device_id":    "SIGAP-01",
		"boot_id":      boot,
		"sequence":     sequence,
		"monotonic_us": monotonic,
		"recorded_at":  nil,
		"time_quality": "UNSYNCED",
		"config_ref": map[string]any{
			"schema_version": 1,
			"config_version": "4",
			"config_hash":    "sha256:" + strings.Repeat("0", 64),
		},
		"profile_refs": []any{},
		"sensor_id":    sensor,
		"stream":       "organic",
		"quality":      "UNCALIBRATED",
		"reason":       "PROFILE_MISSING",
		"raw":          map[string]any{"adc_counts": 1234},
		"converted":    nil,
	}
	if strings.HasPrefix(sensor, "hx711_") {
		record["raw"] = map[string]any{"counts": 1234}
	}
	encoded, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	hash, _, err := telemetry.RecordHash(encoded)
	if err != nil {
		t.Fatal(err)
	}
	record["record_hash"] = hash
	return record
}

func integratedBatch(t *testing.T, batchID string, samples ...map[string]any) []byte {
	t.Helper()
	batch := map[string]any{
		"schema_version": 3,
		"device_id":      "SIGAP-01",
		"batch_id":       batchID,
		"record_count":   len(samples),
		"samples":        samples,
		"results":        []any{},
		"events":         []any{},
		"metadata":       []any{},
	}
	body, err := json.Marshal(batch)
	if err != nil {
		t.Fatal(err)
	}
	return body
}

func TestProductionCompositionKeepsCurrentChronologyAndSensorIdentity(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	token := "current-integration-token"
	digest := sha256.Sum256([]byte(token))
	if _, err := db.Exec(`INSERT INTO locations (id, name) VALUES ('loc-current', 'Current')`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices (id, location_id, device_code, credential_digest) VALUES ('device-current', 'loc-current', 'SIGAP-01', ?)`, hex.EncodeToString(digest[:])); err != nil {
		t.Fatal(err)
	}
	archive := telemetryarchive.NewService(db)
	query, err := telemetryarchive.NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewRouter(ingest.NewService(db), WithTelemetryArchive(archive, query))

	// The newer same-boot record arrives first. A later offline batch contains
	// an older sequence from another boot, so chronology is unresolved rather
	// than silently ordered by server insertion time.
	newBody := integratedBatch(t, "SIGAP-01/boot-a/current",
		integratedSample(t, "boot-a", "9", "mq4", "900"),
		integratedSample(t, "boot-a", "8", "hx711_organic", "800"),
	)
	oldBody := integratedBatch(t, "SIGAP-01/boot-b/backfill",
		integratedSample(t, "boot-b", "1", "mq4", "1"),
	)
	if _, err := telemetry.ValidateBatch(newBody); err != nil {
		t.Fatalf("new integration batch invalid: %v", err)
	}
	if _, err := telemetry.ValidateBatch(oldBody); err != nil {
		t.Fatalf("old integration batch invalid: %v", err)
	}
	post := func(body []byte) {
		t.Helper()
		request := httptest.NewRequest(http.MethodPost, "/api/telemetry/batches", strings.NewReader(string(body)))
		request.Header.Set("Authorization", "Bearer "+token)
		request.Header.Set("Content-Type", "application/json")
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != http.StatusOK {
			t.Fatalf("ingest status=%d body=%s", response.Code, response.Body.String())
		}
	}
	post(newBody)
	post(oldBody)

	request := httptest.NewRequest(http.MethodGet, "/api/telemetry/current/SIGAP-01", nil)
	request.Header.Set("Authorization", "Bearer "+token)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("current status=%d body=%s", response.Code, response.Body.String())
	}
	var snapshot struct {
		Samples         map[string]map[string]json.RawMessage `json:"samples"`
		OrderingUnknown []string                              `json:"ordering_unknown"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil {
		t.Fatal(err)
	}
	if _, ok := snapshot.Samples["organic"]["hx711_organic"]; !ok {
		t.Fatalf("independent HX711 evidence missing: %s", response.Body.Bytes())
	}
	var mq map[string]any
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &mq); err != nil {
		t.Fatal(err)
	}
	if mq["sequence"] != "9" {
		t.Fatalf("late cross-boot backlog replaced current: %#v", mq)
	}
	if len(snapshot.OrderingUnknown) != 1 || snapshot.OrderingUnknown[0] != "organic/mq4" {
		t.Fatalf("ordering uncertainty=%v, want organic/mq4", snapshot.OrderingUnknown)
	}
}
