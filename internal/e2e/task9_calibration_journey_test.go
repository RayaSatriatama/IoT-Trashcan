package e2e_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	devicecontrol "github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	httpapi "github.com/RayaSatriatama/IoT-Trashcan/internal/http"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

// TestTask9RemoteCalibrationLoopbackJourney exercises the production HTTP
// router, SQLite migrations, authorizers, command lease, result persistence,
// and operator readback through an ephemeral TLS server. The device side is
// an explicit synthetic wire-contract seam; it is not ESP32 HIL evidence.
func TestTask9RemoteCalibrationLoopbackJourney(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "task9-control.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	seedControlDevice(t, db, "SIGAP-CAL-01", "device-token")
	now := time.Now().UTC()
	if err := devicecontrol.ProvisionOperatorSession(context.Background(), db,
		"operator-session", "operator-1", "operator-token", "csrf-token",
		now.Add(10*time.Minute), []string{"SIGAP-CAL-01"}); err != nil {
		t.Fatal(err)
	}

	store := devicecontrol.NewStore(db)
	authorizer := devicecontrol.NewDatabaseAuthorizer(db)
	router := httpapi.NewRouter(ingest.NewService(db), httpapi.WithDeviceControl(store,
		devicecontrol.HTTPOptions{Operator: authorizer, Device: authorizer, AllowedOrigin: "https://sigap.test"}))
	server := httptest.NewTLSServer(router)
	defer server.Close()
	client := server.Client()

	const deviceID = "SIGAP-CAL-01"
	const sensorID = "mq4"
	const sessionID = "cal-session-1"
	const profileHash = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	const evidenceHash = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	activeVersion := "7"
	activeProfile := ""
	var committedResultBody []byte

	operations := []struct {
		id      string
		name    string
		payload map[string]any
		status  devicecontrol.Status
		result  map[string]any
	}{
		{"cal-start", "start", map[string]any{"references": map[string]any{"reference_mv": 500}}, devicecontrol.Accepted, map[string]any{"state": "ACTIVE", "session_id": sessionID}},
		{"cal-criteria", "set_criteria", map[string]any{"criteria": map[string]any{"samples": 3, "max_span_counts": 20}}, devicecontrol.Accepted, map[string]any{"state": "CRITERIA_SET", "session_id": sessionID}},
		{"cal-capture", "capture", map[string]any{"anchor": "reference"}, devicecontrol.Capturing, map[string]any{"anchor": "reference", "evidence_hash": evidenceHash, "session_id": sessionID}},
		{"cal-validate", "validate", map[string]any{}, devicecontrol.Validated, map[string]any{"state": "VALIDATED", "evidence_hash": evidenceHash, "session_id": sessionID}},
		{"cal-preview", "preview", map[string]any{}, devicecontrol.PreviewReady, map[string]any{"sensor_id": sensorID, "proposal_hash": profileHash, "profile_version": activeVersion, "evidence_count": 3, "evidence_hash": evidenceHash, "session_id": sessionID}},
		{"cal-commit", "commit", map[string]any{"expected_profile_hash": profileHash, "expected_profile_version": activeVersion}, devicecontrol.Committed, map[string]any{"profile_key": sensorID, "profile_hash": profileHash, "version": 7, "session_id": sessionID}},
	}

	for _, operation := range operations {
		t.Run(operation.name, func(t *testing.T) {
			body, err := json.Marshal(map[string]any{
				"command_id": operation.id, "session_id": sessionID, "sensor_id": sensorID,
				"operation": operation.name, "payload": operation.payload,
				"expires_at": time.Now().UTC().Add(time.Minute).Format(time.RFC3339Nano),
			})
			if err != nil {
				t.Fatal(err)
			}
			created := doControlRequest(t, client, server.URL+"/api/device-control/commands/"+deviceID,
				http.MethodPost, body, "operator-token", "csrf-token", "https://sigap.test")
			if created.StatusCode != http.StatusAccepted {
				t.Fatalf("create status=%d body=%s", created.StatusCode, readBody(created))
			}

			wire := doControlRequest(t, client, server.URL+"/api/device-control/device/"+deviceID+"/next",
				http.MethodGet, nil, "device-token", "", "")
			if wire.StatusCode != http.StatusOK {
				t.Fatalf("next status=%d body=%s", wire.StatusCode, readBody(wire))
			}
			var delivery struct {
				CommandID string          `json:"command_id"`
				SessionID string          `json:"session_id"`
				SensorID  string          `json:"sensor_id"`
				Operation string          `json:"operation"`
				Payload   json.RawMessage `json:"payload"`
			}
			decodeResponse(t, wire, &delivery)
			if delivery.CommandID != operation.id || delivery.SessionID != sessionID || delivery.SensorID != sensorID || delivery.Operation != operation.name {
				t.Fatalf("delivery identity=%+v", delivery)
			}
			resultBody, err := json.Marshal(map[string]any{
				"status": operation.status, "session_id": sessionID, "result": operation.result,
			})
			if err != nil {
				t.Fatal(err)
			}
			result := doControlRequest(t, client, server.URL+"/api/device-control/device/"+deviceID+"/"+operation.id+"/result",
				http.MethodPost, resultBody, "device-token", "", "")
			if result.StatusCode != http.StatusOK {
				t.Fatalf("result status=%d body=%s", result.StatusCode, readBody(result))
			}
			if operation.status == devicecontrol.Committed {
				committedResultBody = append([]byte(nil), resultBody...)
			}
			if operation.status == devicecontrol.Committed {
				activeProfile = profileHash + ":" + activeVersion
			}

			readback := doControlRequest(t, client, server.URL+"/api/device-control/commands/"+deviceID+"/"+operation.id,
				http.MethodGet, nil, "operator-token", "", "")
			if readback.StatusCode != http.StatusOK {
				t.Fatalf("readback status=%d body=%s", readback.StatusCode, readBody(readback))
			}
			var persisted struct {
				Status devicecontrol.Status `json:"status"`
				Result json.RawMessage      `json:"result"`
			}
			decodeResponse(t, readback, &persisted)
			if persisted.Status != operation.status || len(persisted.Result) == 0 {
				t.Fatalf("persisted status=%s result=%s", persisted.Status, persisted.Result)
			}
		})
	}
	if activeProfile != profileHash+":"+activeVersion {
		t.Fatalf("synthetic profile readback=%q", activeProfile)
	}

	// The durable COMMIT result is replay-safe and cannot activate twice.
	replayBody := append([]byte(nil), committedResultBody...)
	replay := doControlRequest(t, client, server.URL+"/api/device-control/device/"+deviceID+"/cal-commit/result", http.MethodPost, replayBody, "device-token", "", "")
	if replay.StatusCode != http.StatusOK {
		t.Fatalf("identical commit replay status=%d body=%s", replay.StatusCode, readBody(replay))
	}
	conflictBody := bytes.Replace(replayBody, []byte(profileHash), []byte("sha256:"+strings.Repeat("c", 64)), 1)
	conflict := doControlRequest(t, client, server.URL+"/api/device-control/device/"+deviceID+"/cal-commit/result", http.MethodPost, conflictBody, "device-token", "", "")
	if conflict.StatusCode != http.StatusConflict {
		t.Fatalf("changed commit replay status=%d body=%s", conflict.StatusCode, readBody(conflict))
	}
	missingCSRF := doControlRequest(t, client, server.URL+"/api/device-control/commands/"+deviceID, http.MethodPost, []byte(`{"command_id":"csrf-fail","session_id":"new","sensor_id":"mq4","operation":"start","payload":{},"expires_at":"2099-01-01T00:00:00Z"}`), "operator-token", "", "https://sigap.test")
	if missingCSRF.StatusCode != http.StatusUnauthorized {
		t.Fatalf("missing csrf status=%d body=%s", missingCSRF.StatusCode, readBody(missingCSRF))
	}
}

func seedControlDevice(t *testing.T, db *sql.DB, deviceID, token string) {
	t.Helper()
	digest := sha256.Sum256([]byte(token))
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES ('task9-control-location','Task 9 control')`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES ('task9-control-device','task9-control-location',?,?)`, deviceID, hex.EncodeToString(digest[:])); err != nil {
		t.Fatal(err)
	}
}

func doControlRequest(t *testing.T, client *http.Client, url, method string, body []byte, token, csrf, origin string) *http.Response {
	t.Helper()
	request, err := http.NewRequestWithContext(context.Background(), method, url, bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	if body != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	if csrf != "" {
		request.Header.Set("X-SIGAP-CSRF", csrf)
	}
	if origin != "" {
		request.Header.Set("Origin", origin)
	}
	response, err := client.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	return response
}

func decodeResponse(t *testing.T, response *http.Response, target any) {
	t.Helper()
	defer response.Body.Close()
	if err := json.NewDecoder(response.Body).Decode(target); err != nil {
		t.Fatal(err)
	}
}

func readBody(response *http.Response) string {
	defer response.Body.Close()
	body, _ := io.ReadAll(response.Body)
	return string(body)
}
