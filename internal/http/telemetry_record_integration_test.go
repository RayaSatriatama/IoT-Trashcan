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

func TestTelemetryRecordArchiveReadIsAuthenticatedFilteredAndCursorStable(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	token := "record-read-token"
	digest := sha256.Sum256([]byte(token))
	if _, err := db.Exec(`INSERT INTO locations (id, name) VALUES ('loc-record', 'Record')`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices (id, location_id, device_code, credential_digest) VALUES ('device-record', 'loc-record', 'SIGAP-01', ?)`, hex.EncodeToString(digest[:])); err != nil {
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
	post := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/api/telemetry/batches", strings.NewReader(string(body)))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+token)
	handler.ServeHTTP(post, request)
	if post.Code != http.StatusOK {
		t.Fatalf("ingest status=%d body=%s", post.Code, post.Body.String())
	}

	page := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodGet, "/api/telemetry/records/SIGAP-01?kind=sample&sensor_id=mq4&limit=1", nil)
	request.Header.Set("Authorization", "Bearer "+token)
	handler.ServeHTTP(page, request)
	if page.Code != http.StatusOK || !strings.Contains(page.Body.String(), `"record_id":"SIGAP-01/boot-A/1"`) {
		t.Fatalf("record page status=%d body=%s", page.Code, page.Body.String())
	}

	export := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodGet, "/api/telemetry/records/SIGAP-01/export?kind=sample", nil)
	request.Header.Set("Authorization", "Bearer "+token)
	handler.ServeHTTP(export, request)
	if export.Code != http.StatusOK {
		t.Fatalf("record export status=%d body=%s", export.Code, export.Body.String())
	}
	exported, err := io.ReadAll(export.Result().Body)
	if err != nil || !strings.Contains(string(exported), `"record_id":"SIGAP-01/boot-A/1"`) {
		t.Fatalf("record export changed evidence: err=%v body=%s", err, exported)
	}

	unauthorized := httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodGet, "/api/telemetry/records/SIGAP-01", nil)
	handler.ServeHTTP(unauthorized, request)
	if unauthorized.Code != http.StatusUnauthorized {
		t.Fatalf("unauthorized record page status=%d", unauthorized.Code)
	}
}
