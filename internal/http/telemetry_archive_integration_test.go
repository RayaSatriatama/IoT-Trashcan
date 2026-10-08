package httpapi

import (
	"crypto/sha256"
	"encoding/hex"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

func TestTelemetryArchiveRoundTripThroughRouter(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	token := "archive-router-token"
	digest := sha256.Sum256([]byte(token))
	if _, err := db.Exec(`INSERT INTO locations (id, name) VALUES ('loc-router', 'Router')`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices (id, location_id, device_code, credential_digest) VALUES ('device-router', 'loc-router', 'SIGAP-01', ?)`, hex.EncodeToString(digest[:])); err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(filepath.Join("..", "..", "contracts", "telemetry", "v3", "body", "body-minimal.json"))
	if err != nil {
		t.Fatal(err)
	}
	archive := telemetryarchive.NewService(db)
	query, err := telemetryarchive.NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewRouter(ingest.NewService(db), WithTelemetryArchive(archive, query))

	request := httptest.NewRequest(http.MethodPost, "/api/telemetry/batches", strings.NewReader(string(body)))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+token)
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("batch status=%d body=%s", recorder.Code, recorder.Body.String())
	}

	replay := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodPost, "/api/telemetry/batches", strings.NewReader(string(body)))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+token)
	handler.ServeHTTP(replay, request)
	if replay.Code != http.StatusOK || !strings.Contains(replay.Body.String(), `"deduplicated_count":1`) {
		t.Fatalf("replay status=%d body=%s", replay.Code, replay.Body.String())
	}

	export := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodGet, "/api/telemetry/batches/SIGAP-01/SIGAP-01/boot-A/batch-1", nil)
	request.Header.Set("Authorization", "Bearer "+token)
	handler.ServeHTTP(export, request)
	if export.Code != http.StatusOK {
		t.Fatalf("export status=%d body=%s", export.Code, export.Body.String())
	}
	readback, err := io.ReadAll(export.Result().Body)
	if err != nil || string(readback) != string(body) {
		t.Fatalf("export changed exact bytes: err=%v got=%q", err, readback)
	}

	list := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodGet, "/api/telemetry/batches/SIGAP-01?limit=1", nil)
	request.Header.Set("Authorization", "Bearer "+token)
	handler.ServeHTTP(list, request)
	if list.Code != http.StatusOK || !strings.Contains(list.Body.String(), `"batch_id":"SIGAP-01/boot-A/batch-1"`) {
		t.Fatalf("list status=%d body=%s", list.Code, list.Body.String())
	}

	current := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodGet, "/api/telemetry/current/SIGAP-01", nil)
	request.Header.Set("Authorization", "Bearer "+token)
	handler.ServeHTTP(current, request)
	if current.Code != http.StatusOK || !strings.Contains(current.Body.String(), `"organic"`) {
		t.Fatalf("current status=%d body=%s", current.Code, current.Body.String())
	}
}
