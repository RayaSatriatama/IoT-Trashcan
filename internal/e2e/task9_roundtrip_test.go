package e2e_test

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	httpapi "github.com/RayaSatriatama/IoT-Trashcan/internal/http"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

// TestTask9TelemetryArchiveRoundTrip exercises the production telemetry-v3
// composition with synthetic, contract-valid sensor evidence. It deliberately
// does not represent physical sensor or network evidence.
func TestTask9TelemetryArchiveRoundTrip(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "task9.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	const deviceID = "SIGAP-01"
	const token = "task9-synthetic-device-token"
	task9SeedArchiveDevice(t, db, deviceID, token)

	archive := telemetryarchive.NewService(db)
	query, err := telemetryarchive.NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	handler := httpapi.NewRouter(ingest.NewService(db), httpapi.WithTelemetryArchive(archive, query))

	fixturePath := filepath.Join("..", "..", "contracts", "telemetry", "v3", "body", "body-minimal.json")
	firstBody, err := os.ReadFile(fixturePath)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := telemetry.ValidateBatch(firstBody); err != nil {
		t.Fatalf("frozen body fixture must remain valid: %v", err)
	}

	first := task9Request(handler, http.MethodPost, "/api/telemetry/batches", firstBody, token)
	if first.Code != http.StatusOK {
		t.Fatalf("first archive POST status=%d body=%s", first.Code, first.Body.String())
	}
	var firstAck telemetry.Ack
	if err := json.Unmarshal(first.Body.Bytes(), &firstAck); err != nil {
		t.Fatal(err)
	}
	if firstAck.Status != "DURABLE" || firstAck.StoredCount != 1 || firstAck.DeduplicatedCount != 0 || firstAck.BodyHash != telemetry.BodyHash(firstBody) {
		t.Fatalf("first durable ACK=%+v", firstAck)
	}

	replay := task9Request(handler, http.MethodPost, "/api/telemetry/batches", firstBody, token)
	if replay.Code != http.StatusOK {
		t.Fatalf("exact replay status=%d body=%s", replay.Code, replay.Body.String())
	}
	var replayAck telemetry.Ack
	if err := json.Unmarshal(replay.Body.Bytes(), &replayAck); err != nil {
		t.Fatal(err)
	}
	if replayAck.StoredCount != 0 || replayAck.DeduplicatedCount != 1 || replayAck.BodyHash != firstAck.BodyHash {
		t.Fatalf("replay ACK=%+v", replayAck)
	}

	// A valid JSON body with the same batch identity but different immutable
	// bytes is a conflict, including a whitespace-only body change.
	conflictingBody := append(append([]byte(nil), firstBody...), '\n')
	if _, err := telemetry.ValidateBatch(conflictingBody); err != nil {
		t.Fatalf("whitespace-conflicting body must remain contract-valid: %v", err)
	}
	conflict := task9Request(handler, http.MethodPost, "/api/telemetry/batches", conflictingBody, token)
	if conflict.Code != http.StatusConflict {
		t.Fatalf("conflicting replay status=%d body=%s", conflict.Code, conflict.Body.String())
	}

	unauthorized := task9Request(handler, http.MethodPost, "/api/telemetry/batches", firstBody, "wrong-synthetic-token")
	if unauthorized.Code != http.StatusUnauthorized {
		t.Fatalf("wrong bearer status=%d body=%s", unauthorized.Code, unauthorized.Body.String())
	}

	export := task9Request(handler, http.MethodGet, "/api/telemetry/batches/SIGAP-01/SIGAP-01/boot-A/batch-1", nil, token)
	if export.Code != http.StatusOK {
		t.Fatalf("exact batch export status=%d body=%s", export.Code, export.Body.String())
	}
	exported, err := io.ReadAll(export.Result().Body)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(exported, firstBody) {
		t.Fatalf("exact batch export changed immutable bytes\nwant=%q\n got=%q", firstBody, exported)
	}

	// A newer sample arrives first. The late same-boot backlog must remain in
	// the archive but cannot replace latest source evidence in current state.
	newestBody := task9Batch(t, "SIGAP-01/boot-A/newest", task9Sample(t, "boot-A", "3", "300"))
	backlogBody := task9Batch(t, "SIGAP-01/boot-A/backlog", task9Sample(t, "boot-A", "2", "200"))
	for _, body := range [][]byte{newestBody, backlogBody} {
		response := task9Request(handler, http.MethodPost, "/api/telemetry/batches", body, token)
		if response.Code != http.StatusOK {
			t.Fatalf("synthetic archive POST status=%d body=%s", response.Code, response.Body.String())
		}
	}

	current := task9Request(handler, http.MethodGet, "/api/telemetry/current/SIGAP-01", nil, token)
	if current.Code != http.StatusOK {
		t.Fatalf("current status=%d body=%s", current.Code, current.Body.String())
	}
	var snapshot struct {
		Samples map[string]map[string]json.RawMessage `json:"samples"`
	}
	if err := json.Unmarshal(current.Body.Bytes(), &snapshot); err != nil {
		t.Fatal(err)
	}
	var latest struct {
		Sequence string `json:"sequence"`
	}
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &latest); err != nil {
		t.Fatalf("decode current mq4: %v body=%s", err, current.Body.String())
	}
	if latest.Sequence != "3" {
		t.Fatalf("late backlog regressed current mq4 sequence=%q, want 3", latest.Sequence)
	}

	page := task9Request(handler, http.MethodGet, "/api/telemetry/records/SIGAP-01?kind=sample&stream=organic&sensor_id=mq4&limit=1", nil, token)
	if page.Code != http.StatusOK {
		t.Fatalf("filtered page status=%d body=%s", page.Code, page.Body.String())
	}
	var firstPage struct {
		Records []struct {
			RecordID string `json:"record_id"`
		} `json:"records"`
		NextCursor string `json:"next_cursor"`
	}
	if err := json.Unmarshal(page.Body.Bytes(), &firstPage); err != nil {
		t.Fatal(err)
	}
	if len(firstPage.Records) != 1 || firstPage.Records[0].RecordID != "SIGAP-01/boot-A/1" || firstPage.NextCursor == "" {
		t.Fatalf("first filtered cursor page=%s", page.Body.String())
	}

	continued := task9Request(handler, http.MethodGet, "/api/telemetry/records/SIGAP-01?kind=sample&stream=organic&sensor_id=mq4&limit=1&cursor="+firstPage.NextCursor, nil, token)
	if continued.Code != http.StatusOK {
		t.Fatalf("continued cursor page status=%d body=%s", continued.Code, continued.Body.String())
	}
	var secondPage struct {
		Records []struct {
			RecordID string `json:"record_id"`
		} `json:"records"`
	}
	if err := json.Unmarshal(continued.Body.Bytes(), &secondPage); err != nil {
		t.Fatal(err)
	}
	if len(secondPage.Records) != 1 || secondPage.Records[0].RecordID != "SIGAP-01/boot-A/2" {
		t.Fatalf("stable filtered cursor continuation=%s", continued.Body.String())
	}

	deniedExport := task9Request(handler, http.MethodGet, "/api/telemetry/records/SIGAP-01/export?kind=sample", nil, "wrong-synthetic-token")
	if deniedExport.Code != http.StatusUnauthorized {
		t.Fatalf("wrong bearer record export status=%d body=%s", deniedExport.Code, deniedExport.Body.String())
	}
}

func task9SeedArchiveDevice(t *testing.T, db *sql.DB, deviceID, token string) {
	t.Helper()
	digest := sha256.Sum256([]byte(token))
	if _, err := db.Exec(`INSERT INTO locations (id, name) VALUES ('task9-location', 'Task 9 synthetic')`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices (id, location_id, device_code, credential_digest) VALUES ('task9-device', 'task9-location', ?, ?)`, deviceID, hex.EncodeToString(digest[:])); err != nil {
		t.Fatal(err)
	}
}

func task9Request(handler http.Handler, method, path string, body []byte, token string) *httptest.ResponseRecorder {
	request := httptest.NewRequest(method, path, bytes.NewReader(body))
	if method == http.MethodPost {
		request.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}

func task9Sample(t *testing.T, bootID, sequence, monotonicUS string) map[string]any {
	t.Helper()
	record := map[string]any{
		"record_id":    "SIGAP-01/" + bootID + "/" + sequence,
		"record_hash":  "sha256:" + strings.Repeat("0", 64),
		"kind":         "sample",
		"device_id":    "SIGAP-01",
		"boot_id":      bootID,
		"sequence":     sequence,
		"monotonic_us": monotonicUS,
		"recorded_at":  nil,
		"time_quality": "UNSYNCED",
		"config_ref": map[string]any{
			"schema_version": 1,
			"config_version": "4",
			"config_hash":    "sha256:" + strings.Repeat("0", 64),
		},
		"profile_refs": []any{},
		"sensor_id":    "mq4",
		"stream":       "organic",
		"quality":      "UNCALIBRATED",
		"reason":       "PROFILE_MISSING",
		"raw":          map[string]any{"adc_counts": 1234},
		"converted":    nil,
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

func task9Batch(t *testing.T, batchID string, samples ...map[string]any) []byte {
	t.Helper()
	body, err := json.Marshal(map[string]any{
		"schema_version": 3,
		"device_id":      "SIGAP-01",
		"batch_id":       batchID,
		"record_count":   len(samples),
		"samples":        samples,
		"results":        []any{},
		"events":         []any{},
		"metadata":       []any{},
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := telemetry.ValidateBatch(body); err != nil {
		t.Fatalf("synthetic Task 9 body must be contract-valid: %v", err)
	}
	return body
}
