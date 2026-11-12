package httpapi

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicedebug"
)

// All identities, credentials and databases in this file are disposable TEST data.
func task9HTTPFixture(t *testing.T, policy devicedebug.Policy) *authHTTPFixture {
	t.Helper()
	f := newAuthHTTPFixture(t)
	store, err := devicedebug.NewStore(f.db, policy)
	if err != nil {
		t.Fatal(err)
	}
	store.WithClock(func() time.Time { return f.now })
	auth := devicecontrol.NewDatabaseAuthorizer(f.db).WithClock(func() time.Time { return f.now })
	f.router = NewRouter(nil, WithDeviceDebug(store, devicedebug.HTTPOptions{Device: auth, Operator: auth}))
	return f
}

func task9Producer(t *testing.T) ([]byte, devicedebug.Upload) {
	t.Helper()
	stored, err := os.ReadFile("../../contracts/debug/v1/fixtures/http.json")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasSuffix(stored, []byte("\n")) || bytes.HasSuffix(stored, []byte("\n\n")) {
		t.Fatal("producer fixture must have exactly one storage-only final LF")
	}
	raw := bytes.TrimSuffix(stored, []byte("\n"))
	upload, err := devicedebug.DecodeUpload(raw)
	if err != nil {
		t.Fatal(err)
	}
	return raw, upload
}

func task9Body(t *testing.T, value any) []byte {
	t.Helper()
	raw, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func task9Request(t *testing.T, f *authHTTPFixture, method, path, token string, body []byte, status int) *httptest.ResponseRecorder {
	t.Helper()
	w := f.request(method, path, token, body)
	if w.Code != status {
		t.Fatalf("%s %s: status=%d want=%d body=%s", method, path, w.Code, status, w.Body)
	}
	return w
}

func task9Page(t *testing.T, response *httptest.ResponseRecorder) devicedebug.HistoryPage {
	t.Helper()
	var page devicedebug.HistoryPage
	if err := json.Unmarshal(response.Body.Bytes(), &page); err != nil {
		t.Fatal(err)
	}
	return page
}

func task9DatabaseSnapshot(t *testing.T, db *sql.DB) string {
	t.Helper()
	queries := []string{
		`SELECT ingestion_id,device_code,boot_id,sequence_text,event_json,build_id,received_at,snapshot_level,captured_monotonic_us,oldest_sequence,overwritten,delivery_lost FROM device_debug_history ORDER BY ingestion_id`,
		`SELECT device_code,pruned_through FROM device_debug_watermarks ORDER BY device_code`,
	}
	var snapshot [][][]any
	for _, query := range queries {
		rows, err := db.Query(query)
		if err != nil {
			t.Fatal(err)
		}
		columns, err := rows.Columns()
		if err != nil {
			rows.Close()
			t.Fatal(err)
		}
		values := make([][]any, 0)
		for rows.Next() {
			row := make([]any, len(columns))
			pointers := make([]any, len(columns))
			for i := range row {
				pointers[i] = &row[i]
			}
			if err := rows.Scan(pointers...); err != nil {
				rows.Close()
				t.Fatal(err)
			}
			for i, value := range row {
				if text, ok := value.([]byte); ok {
					row[i] = string(text)
				}
			}
			values = append(values, row)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			t.Fatal(err)
		}
		if err := rows.Close(); err != nil {
			t.Fatal(err)
		}
		snapshot = append(snapshot, values)
	}
	return string(task9Body(t, snapshot))
}

func TestTask9ActualProducerAuthenticatedHistoryFixture(t *testing.T) {
	f := task9HTTPFixture(t, devicedebug.DefaultPolicy())
	raw, upload := task9Producer(t)
	getPath := "/api/device-debug/events/TEST-DEVICE"
	postPath := "/api/device-debug/device/TEST-DEVICE/events"
	empty := task9Request(t, f, http.MethodGet, getPath, "operator-token", nil, http.StatusOK)
	if page := task9Page(t, empty); len(page.Events) != 0 || page.NextAfter != "0" || page.RetentionGap {
		t.Fatalf("initial history: %+v", page)
	}
	ackResponse := task9Request(t, f, http.MethodPost, postPath, "device-token", raw, http.StatusOK)
	var ack devicedebug.UploadAck
	if err := json.Unmarshal(ackResponse.Body.Bytes(), &ack); err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(raw)
	if ack.DeviceID != upload.DeviceID || ack.BootID != upload.BootID || ack.FirstSequence != upload.Events[0].Sequence || ack.LastSequence != upload.Events[len(upload.Events)-1].Sequence || ack.RequestSHA256 != hex.EncodeToString(digest[:]) || ack.StoredCount != 1 || ack.DeduplicatedCount != 0 {
		t.Fatalf("ACK does not bind exact producer payload: %+v", ack)
	}
	history := task9Request(t, f, http.MethodGet, getPath, "operator-token", nil, http.StatusOK)
	page := task9Page(t, history)
	if page.DeviceID != upload.DeviceID || len(page.Events) != 1 || !reflect.DeepEqual(page.Events[0].DebugEvent, upload.Events[0]) || page.Events[0].BootID != upload.BootID || page.Events[0].BuildID != upload.BuildID || page.Events[0].CapturedMonotonicUS != upload.CapturedMonotonicUS || page.Events[0].ReceivedAt != f.now.UTC().Format("2006-01-02T15:04:05.000000000Z") {
		t.Fatalf("durable history differs from producer or server receipt: %+v", page)
	}
	tail := task9Request(t, f, http.MethodGet, getPath+"?after="+page.NextAfter, "operator-token", nil, http.StatusOK)
	if got := task9Page(t, tail); len(got.Events) != 0 || got.NextAfter != page.NextAfter || got.HasMore {
		t.Fatalf("empty continuation cursor changed: %+v", got)
	}
	before := task9DatabaseSnapshot(t, f.db)
	dedup := task9Request(t, f, http.MethodPost, postPath, "device-token", raw, http.StatusOK)
	var duplicate devicedebug.UploadAck
	if err := json.Unmarshal(dedup.Body.Bytes(), &duplicate); err != nil {
		t.Fatal(err)
	}
	if duplicate.StoredCount != 0 || duplicate.DeduplicatedCount != 1 || duplicate.RequestSHA256 != ack.RequestSHA256 || before != task9DatabaseSnapshot(t, f.db) {
		t.Fatal("duplicate changed persisted identity, first receipt, metadata or digest")
	}

	// A new boot is an explicit TEST mutation of the valid producer envelope.
	nextBoot := upload
	nextBoot.BootID = "00000000000000000000000000000002"
	task9Request(t, f, http.MethodPost, postPath, "device-token", task9Body(t, nextBoot), http.StatusOK)
	bootHistory := task9Request(t, f, http.MethodGet, getPath, "operator-token", nil, http.StatusOK)
	boots := task9Page(t, bootHistory)
	if len(boots.Events) != 2 || boots.Events[0].BootID == boots.Events[1].BootID || boots.Events[0].Sequence != boots.Events[1].Sequence {
		t.Fatal("boot identity was conflated with source sequence")
	}

	retained := task9HTTPFixture(t, devicedebug.Policy{Retention: 24 * time.Hour, PerDeviceLimit: 1, GlobalLimit: 2})
	task9Request(t, retained, http.MethodPost, postPath, "device-token", raw, http.StatusOK)
	task9Request(t, retained, http.MethodPost, postPath, "device-token", task9Body(t, nextBoot), http.StatusOK)
	retentionHistory := task9Request(t, retained, http.MethodGet, getPath, "operator-token", nil, http.StatusOK)
	retention := task9Page(t, retentionHistory)
	if !retention.RetentionGap || len(retention.Events) != 1 || retention.Events[0].BootID != nextBoot.BootID || retention.OldestAvailable != retention.Events[0].IngestionID {
		t.Fatalf("retention provenance missing: %+v", retention)
	}

	if _, err := f.db.Exec(`UPDATE device_control_operator_sessions SET expires_at=? WHERE session_id='session'`, f.now.Format(time.RFC3339Nano)); err != nil {
		t.Fatal(err)
	}
	expired := task9Request(t, f, http.MethodGet, getPath, "operator-token", nil, http.StatusUnauthorized)
	fixture := map[string]any{
		"provenance": "TEST", "device_id": upload.DeviceID,
		"producer_sha256":         hex.EncodeToString(digest[:]),
		"ack":                     json.RawMessage(ackResponse.Body.Bytes()),
		"history":                 json.RawMessage(history.Body.Bytes()),
		"tail_history":            json.RawMessage(tail.Body.Bytes()),
		"empty_history":           json.RawMessage(empty.Body.Bytes()),
		"boot_change_history":     json.RawMessage(bootHistory.Body.Bytes()),
		"retention_history":       json.RawMessage(retentionHistory.Body.Bytes()),
		"expired_operator_status": expired.Code,
		"expired_operator_body":   json.RawMessage(expired.Body.Bytes()),
	}
	encoded, err := json.MarshalIndent(fixture, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	encoded = append(encoded, '\n')
	fixturePath := filepath.Join("..", "..", "tests", "fixtures", "debug-production-history.json")
	if os.Getenv("SIGAP_UPDATE_DEBUG_HISTORY_FIXTURE") == "1" {
		if err := os.WriteFile(fixturePath, encoded, 0644); err != nil {
			t.Fatal(err)
		}
	}
	frozen, err := os.ReadFile(fixturePath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(frozen, encoded) {
		t.Fatal("actual authenticated handler output differs from frozen TEST fixture")
	}
}

func TestTask9ProducerIdentityAndConflictRollback(t *testing.T) {
	f := task9HTTPFixture(t, devicedebug.DefaultPolicy())
	raw, upload := task9Producer(t)
	postPath := "/api/device-debug/device/TEST-DEVICE/events"
	task9Request(t, f, http.MethodPost, postPath, "device-token", raw, http.StatusOK)
	before := task9DatabaseSnapshot(t, f.db)
	conflicting := upload
	first := upload.Events[0]
	first.Sequence = "1"
	conflict := upload.Events[0]
	conflict.Arg0++
	conflicting.Events = []devicedebug.DebugEvent{first, conflict}
	candidate := task9Body(t, conflicting)
	if _, err := devicedebug.DecodeUpload(candidate); err != nil {
		t.Fatalf("rollback fixture must be schema-valid: %v", err)
	}
	task9Request(t, f, http.MethodPost, postPath, "device-token", candidate, http.StatusConflict)
	if before != task9DatabaseSnapshot(t, f.db) {
		t.Fatal("later identity conflict did not roll back earlier insertion/watermark state")
	}
	wrongBody := upload
	wrongBody.DeviceID = "OTHER-DEVICE"
	task9Request(t, f, http.MethodPost, postPath, "device-token", task9Body(t, wrongBody), http.StatusBadRequest)
	task9Request(t, f, http.MethodPost, "/api/device-debug/device/OTHER-DEVICE/events", "device-token", raw, http.StatusUnauthorized)
	task9Request(t, f, http.MethodPost, postPath, "wrong-token", raw, http.StatusUnauthorized)
	task9Request(t, f, http.MethodGet, "/api/device-debug/events/OTHER-DEVICE", "operator-token", nil, http.StatusUnauthorized)
	if before != task9DatabaseSnapshot(t, f.db) {
		t.Fatal("rejected identity/authorization changed stored content")
	}
}
