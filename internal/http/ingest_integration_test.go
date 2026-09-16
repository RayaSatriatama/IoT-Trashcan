package httpapi

import (
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

const integrationPayload = `{"device_id":"SIGAP-01","recorded_at":"2026-09-16T10:00:00Z","health":{"battery":90},"slots":[{"stream":"organic","fill":1},{"stream":"inorganic","fill":2},{"stream":"b3","fill":3}]}`

func TestIngestEndpointPersistsAndRejectsWrongCredential(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	digest := sha256.Sum256([]byte("fixture-token"))
	if _, err := db.Exec(`INSERT INTO locations (id, name) VALUES ('location-1', 'Test')`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices (id, location_id, device_code, credential_digest) VALUES ('device-1', 'location-1', 'SIGAP-01', ?)`, hex.EncodeToString(digest[:])); err != nil {
		t.Fatal(err)
	}
	for _, stream := range []string{"organic", "inorganic", "b3"} {
		if _, err := db.Exec(`INSERT INTO slots (id, device_id, waste_stream) VALUES (?, 'device-1', ?)`, "slot-"+stream, stream); err != nil {
			t.Fatal(err)
		}
	}

	handler := NewRouter(ingest.NewService(db))
	request := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(integrationPayload))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer fixture-token")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("valid ingest status=%d body=%s", response.Code, response.Body.String())
	}

	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM readings`).Scan(&count); err != nil || count != 3 {
		t.Fatalf("reading count=%d err=%v", count, err)
	}

	badRequest := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(integrationPayload))
	badRequest.Header.Set("Content-Type", "application/json")
	badRequest.Header.Set("Authorization", "Bearer wrong-token")
	badResponse := httptest.NewRecorder()
	handler.ServeHTTP(badResponse, badRequest)
	if badResponse.Code != http.StatusUnauthorized {
		t.Fatalf("wrong credential status=%d body=%s", badResponse.Code, badResponse.Body.String())
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM readings`).Scan(&count); err != nil || count != 3 {
		t.Fatalf("wrong credential altered reading count=%d err=%v", count, err)
	}
}
