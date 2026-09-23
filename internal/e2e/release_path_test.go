package e2e_test

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/fuzzy"
	httpapi "github.com/RayaSatriatama/IoT-Trashcan/internal/http"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/safety"
)

const e2eToken = "e2e-fixture-token"

func TestReleasePathIngestReadHistoryCSVAndRestart(t *testing.T) {
	dbPath := filepath.Join(t.TempDir(), "sigap.db")
	db, err := database.Open(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	seedDevice(t, db)

	engine := newCountingEngine(t)
	service := ingest.NewService(db,
		ingest.WithDecisionEngine(engine),
		ingest.WithSafetyConfig(safety.Config{Thresholds: []safety.Threshold{{
			Signal: safety.SignalMQ7CO, AtOrAbove: 0.8, Reason: "test-only B3 CO fixture",
		}}}),
	)
	handler := httpapi.NewRouter(service, httpapi.WithReadDatabase(db,
		httpapi.WithOfflineAfter(time.Minute),
		httpapi.WithReadClock(func() time.Time { return time.Date(2026, 9, 17, 12, 1, 0, 0, time.UTC) }),
	))

	validRaw := []byte(`{"device_id":"SIGAP-E2E","recorded_at":"2026-09-17T12:00:00Z","health":{"battery":91},"slots":[{"stream":"organic","fill":72,"weight":12},{"stream":"inorganic","fill":40,"weight":4},{"stream":"b3","fill":10,"weight":2,"gas_raw":99,"sensors":[{"sensor_id":"b3-mq7","sensor_type":"mq7","parameter":"co","raw_value":99,"normalized_value":0.99,"unit":"index","sensor_state":"VALID"}]}]}`)
	post := ingestRequest(t, handler, validRaw, e2eToken)
	if post.Code != http.StatusOK {
		t.Fatalf("valid ingest status=%d body=%s", post.Code, post.Body.String())
	}
	assertCount(t, db, "readings", 3)
	assertCount(t, db, "decisions", 3)
	var version, reason, safetyState string
	if err := db.QueryRow(`SELECT fis_version, dominant_reason, safety_state FROM decisions WHERE reading_id = 3`).Scan(&version, &reason, &safetyState); err != nil {
		t.Fatal(err)
	}
	if version != "e2e-fis-v1" || reason == "" || safetyState != safety.StateSafetyAlert {
		t.Fatalf("decision version=%q reason=%q safety=%q", version, reason, safetyState)
	}
	if got := engine.calls.Load(); got != 3 {
		t.Fatalf("FIS calls after valid ingest=%d, want 3", got)
	}

	assertJSONEndpoint(t, handler, "/api/overview", func(body map[string]any) {
		if body["total_devices"] != float64(1) || body["safety_alerts"] != float64(1) {
			t.Fatalf("overview=%v", body)
		}
	})
	assertJSONEndpoint(t, handler, "/api/locations/location-e2e", func(body map[string]any) {
		if body["status"] != string("Safety Alert") {
			t.Fatalf("location=%v", body)
		}
	})
	assertJSONEndpoint(t, handler, "/api/devices/device-e2e", func(body map[string]any) {
		if body["status"] != string("Safety Alert") || strings.Contains(stringValue(body), "credential_digest") {
			t.Fatalf("device response=%v", body)
		}
	})
	assertJSONEndpoint(t, handler, "/api/devices/device-e2e/research", func(body map[string]any) {
		if strings.Contains(stringValue(body), "ppm") || !strings.Contains(stringValue(body), `"gas_raw":99`) {
			t.Fatalf("research response=%v", body)
		}
	})
	assertJSONEndpoint(t, handler, "/api/history?status=Safety%20Alert", func(body map[string]any) {
		items, ok := body["items"].([]any)
		if !ok || len(items) != 1 || strings.Contains(stringValue(body), "credential_digest") {
			t.Fatalf("history response=%v", body)
		}
	})
	export := request(t, handler, http.MethodGet, "/api/export.csv?status=Safety%20Alert", nil, "")
	if export.Code != http.StatusOK || !strings.Contains(export.Header().Get("Content-Type"), "text/csv") || !strings.Contains(export.Body.String(), "gas_raw") || !strings.Contains(export.Body.String(), "99") {
		t.Fatalf("CSV status=%d headers=%v body=%s", export.Code, export.Header(), export.Body.String())
	}
	if strings.Contains(export.Body.String(), "credential_digest") || strings.Contains(export.Body.String(), e2eToken) {
		t.Fatalf("CSV leaked secret: %s", export.Body.String())
	}

	// Exact replay is idempotent; the same timestamp with different raw data is explicit conflict.
	replay := ingestRequest(t, handler, validRaw, e2eToken)
	if replay.Code != http.StatusOK || !strings.Contains(replay.Body.String(), `"deduplicated":true`) {
		t.Fatalf("replay status=%d body=%s", replay.Code, replay.Body.String())
	}
	assertCount(t, db, "readings", 3)
	conflicting := append([]byte(nil), validRaw...)
	conflicting = []byte(strings.Replace(string(conflicting), `"battery":91`, `"battery":90`, 1))
	conflict := ingestRequest(t, handler, conflicting, e2eToken)
	if conflict.Code != http.StatusConflict {
		t.Fatalf("conflicting replay status=%d body=%s", conflict.Code, conflict.Body.String())
	}
	assertCount(t, db, "readings", 3)

	// Wrong credentials do not create any new data.
	wrong := []byte(strings.Replace(string(validRaw), "12:00:00Z", "12:00:01Z", 1))
	unauthorized := ingestRequest(t, handler, wrong, "wrong-token")
	if unauthorized.Code != http.StatusUnauthorized {
		t.Fatalf("wrong token status=%d body=%s", unauthorized.Code, unauthorized.Body.String())
	}
	assertCount(t, db, "readings", 3)

	// Invalid values are auditable but bypass the decision engine and never become Normal.
	invalidRaw := []byte(strings.Replace(string(validRaw), "12:00:00Z", "12:00:02Z", 1))
	invalidRaw = []byte(strings.Replace(string(invalidRaw), `"fill":72`, `"fill":101`, 1))
	invalidRaw = []byte(strings.Replace(string(invalidRaw), `"fill":40`, `"fill":101`, 1))
	invalidRaw = []byte(strings.Replace(string(invalidRaw), `"fill":10,"weight":2`, `"fill":101,"weight":2`, 1))
	invalid := ingestRequest(t, handler, invalidRaw, e2eToken)
	if invalid.Code != http.StatusOK {
		t.Fatalf("invalid ingest status=%d body=%s", invalid.Code, invalid.Body.String())
	}
	if got := engine.calls.Load(); got != 3 {
		t.Fatalf("FIS calls after invalid ingest=%d, want 3", got)
	}
	var invalidStatus string
	var invalidScore sql.NullFloat64
	if err := db.QueryRow(`SELECT fis_status, fis_score FROM decisions WHERE reading_id = 4`).Scan(&invalidStatus, &invalidScore); err != nil {
		t.Fatal(err)
	}
	if invalidStatus != "Data Invalid" || invalidScore.Valid {
		t.Fatalf("invalid decision status=%q score=%#v", invalidStatus, invalidScore)
	}

	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	// Reopening the same file is the process-restart persistence check.
	reopened, err := database.Open(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	restartedHandler := httpapi.NewRouter(ingest.NewService(reopened), httpapi.WithReadDatabase(reopened))
	assertJSONEndpoint(t, restartedHandler, "/api/history", func(body map[string]any) {
		items, ok := body["items"].([]any)
		if !ok || len(items) != 6 {
			t.Fatalf("history after restart=%v", body)
		}
	})
}

type countingEngine struct {
	inner fuzzy.Engine
	calls atomic.Int32
}

func (e *countingEngine) Evaluate(input fuzzy.Input) (fuzzy.Result, error) {
	e.calls.Add(1)
	return e.inner.Evaluate(input)
}

func (e *countingEngine) Version() string { return e.inner.Version() }

func newCountingEngine(t *testing.T) *countingEngine {
	t.Helper()
	engine, err := fuzzy.NewEngine(fuzzy.Config{
		Version: "e2e-fis-v1",
		Inputs: map[string]fuzzy.InputVariable{
			"fill":               {Sets: map[string]fuzzy.Membership{"any": fuzzy.TrapezoidMembership(0, 0, 100, 100)}},
			"weight_utilization": {Sets: map[string]fuzzy.Membership{"any": fuzzy.TrapezoidMembership(0, 0, 500, 500)}},
		},
		Outputs: map[string]fuzzy.OutputSet{
			"priority": {Status: "Waspada", Membership: fuzzy.TriangleMembership(0, 50, 100)},
		},
		Rules:       []fuzzy.Rule{{ID: "e2e-any", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "any"}}, Output: "priority", Reason: "test-only FIS fixture"}},
		OutputRange: fuzzy.Range{Min: 0, Max: 100, Step: 1},
	})
	if err != nil {
		t.Fatal(err)
	}
	return &countingEngine{inner: engine}
}

func seedDevice(t *testing.T, db *sql.DB) {
	t.Helper()
	digest := sha256.Sum256([]byte(e2eToken))
	if _, err := db.Exec(`INSERT INTO locations (id, name, address_text) VALUES ('location-e2e', 'E2E Campus', 'Test address')`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices (id, location_id, device_code, placement_label, credential_digest) VALUES ('device-e2e', 'location-e2e', 'SIGAP-E2E', 'Lobby', ?)`, hex.EncodeToString(digest[:])); err != nil {
		t.Fatal(err)
	}
	for _, stream := range []string{"organic", "inorganic", "b3"} {
		if _, err := db.Exec(`INSERT INTO slots (id, device_id, waste_stream, operational_capacity) VALUES (?, 'device-e2e', ?, 20)`, "slot-e2e-"+stream, stream); err != nil {
			t.Fatal(err)
		}
	}
}

func ingestRequest(t *testing.T, handler http.Handler, body []byte, token string) *httptest.ResponseRecorder {
	t.Helper()
	return request(t, handler, http.MethodPost, "/api/ingest", body, token)
}

func request(t *testing.T, handler http.Handler, method, path string, body []byte, token string) *httptest.ResponseRecorder {
	t.Helper()
	var reader io.Reader
	if body != nil {
		reader = strings.NewReader(string(body))
	}
	req := httptest.NewRequest(method, path, reader)
	if method == http.MethodPost {
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Authorization", "Bearer "+token)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, req)
	return response
}

func assertJSONEndpoint(t *testing.T, handler http.Handler, path string, check func(map[string]any)) {
	t.Helper()
	response := request(t, handler, http.MethodGet, path, nil, "")
	if response.Code != http.StatusOK {
		t.Fatalf("GET %s status=%d body=%s", path, response.Code, response.Body.String())
	}
	var body map[string]any
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("GET %s JSON: %v", path, err)
	}
	check(body)
}

func stringValue(value any) string {
	encoded, _ := json.Marshal(value)
	return string(encoded)
}

func assertCount(t *testing.T, db *sql.DB, table string, want int) {
	t.Helper()
	var got int
	if err := db.QueryRow(`SELECT COUNT(*) FROM ` + table).Scan(&got); err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Fatalf("%s count=%d, want %d", table, got, want)
	}
}
