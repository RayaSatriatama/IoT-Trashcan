package devicedebug

import (
	"database/sql"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
	"time"
)

type transactionSnapshot struct {
	Rows       [][]any
	Watermarks [][]any
}

func snapshotHTTPTransaction(t *testing.T, db interface {
	Query(string, ...any) (*sql.Rows, error)
}) transactionSnapshot {
	t.Helper()
	read := func(query string) [][]any {
		rows, err := db.Query(query)
		if err != nil {
			t.Fatal(err)
		}
		defer rows.Close()
		columns, err := rows.Columns()
		if err != nil {
			t.Fatal(err)
		}
		out := make([][]any, 0)
		for rows.Next() {
			values := make([]any, len(columns))
			pointers := make([]any, len(columns))
			for i := range values {
				pointers[i] = &values[i]
			}
			if err := rows.Scan(pointers...); err != nil {
				t.Fatal(err)
			}
			for i, value := range values {
				if bytes, ok := value.([]byte); ok {
					values[i] = string(bytes)
				}
			}
			out = append(out, values)
		}
		if err := rows.Err(); err != nil {
			t.Fatal(err)
		}
		return out
	}
	return transactionSnapshot{
		Rows:       read(`SELECT ingestion_id,device_code,boot_id,sequence_text,event_json,build_id,received_at,snapshot_level,captured_monotonic_us,oldest_sequence,overwritten,delivery_lost FROM device_debug_history ORDER BY ingestion_id`),
		Watermarks: read(`SELECT device_code,pruned_through FROM device_debug_watermarks ORDER BY device_code`),
	}
}

func postHTTPTransaction(t *testing.T, handler http.Handler, upload Upload) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events", strings.NewReader(string(bodyFor(t, upload))))
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	return w
}

func assertHTTPTransactionFailure(t *testing.T, w *httptest.ResponseRecorder, status int) {
	t.Helper()
	if w.Code != status || w.Body.String() != "{\"error\":\"internal server error\"}\n" && status == http.StatusInternalServerError || w.Code != status || strings.Contains(w.Body.String(), "DURABLE") || strings.Contains(w.Body.String(), "trigger") || strings.Contains(w.Body.String(), "SQLITE") {
		t.Fatalf("status=%d body=%q", w.Code, w.Body.String())
	}
}

func TestHTTPAtomicLaterContentConflictRollsBackEarlierEvent(t *testing.T) {
	store, db, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Device: testAuthorizer{}})
	boot := "00000000000000000000000000000032"
	seed := makeUpload("TEST-DEVICE", boot, "old-build", 4, 1)
	if w := postHTTPTransaction(t, h, seed); w.Code != http.StatusOK {
		t.Fatalf("seed status=%d body=%s", w.Code, w.Body)
	}
	before := snapshotHTTPTransaction(t, db)
	candidate := makeUpload("TEST-DEVICE", boot, "new-build", 3, 2)
	if _, err := DecodeUpload(bodyFor(t, candidate)); err != nil {
		t.Fatalf("candidate invalid: %v", err)
	}
	w := postHTTPTransaction(t, h, candidate)
	if w.Code != http.StatusConflict || !strings.Contains(w.Body.String(), "content conflict") || strings.Contains(w.Body.String(), "DURABLE") {
		t.Fatalf("status=%d body=%q", w.Code, w.Body.String())
	}
	after := snapshotHTTPTransaction(t, db)
	if !reflect.DeepEqual(after, before) {
		t.Fatalf("state changed\nbefore=%+v\nafter=%+v", before, after)
	}
	if len(after.Rows) != 1 || after.Rows[0][3] != "4" || after.Rows[0][5] != "old-build" {
		t.Fatalf("original row not preserved: %+v", after.Rows)
	}
}

func TestHTTPTransactionLaterSQLiteInsertErrorRollsBackBatch(t *testing.T) {
	store, db, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Device: testAuthorizer{}})
	candidate := makeUpload("TEST-DEVICE", "00000000000000000000000000000033", "build", 10, 2)
	if _, err := DecodeUpload(bodyFor(t, candidate)); err != nil {
		t.Fatalf("candidate invalid: %v", err)
	}
	if _, err := db.Exec(`CREATE TRIGGER fail_later_debug_insert BEFORE INSERT ON device_debug_history WHEN NEW.sequence_text='11' BEGIN SELECT RAISE(FAIL,'trigger-secret'); END`); err != nil {
		t.Fatal(err)
	}
	before := snapshotHTTPTransaction(t, db)
	assertHTTPTransactionFailure(t, postHTTPTransaction(t, h, candidate), http.StatusInternalServerError)
	if after := snapshotHTTPTransaction(t, db); !reflect.DeepEqual(after, before) {
		t.Fatalf("state changed\nbefore=%+v\nafter=%+v", before, after)
	}
	if _, err := db.Exec(`DROP TRIGGER fail_later_debug_insert`); err != nil {
		t.Fatal(err)
	}
	w := postHTTPTransaction(t, h, candidate)
	if w.Code != http.StatusOK || !strings.Contains(w.Body.String(), "DURABLE") {
		t.Fatalf("valid positive baseline status=%d body=%q", w.Code, w.Body.String())
	}
}

func TestHTTPTransactionPruningWatermarkErrorRollsBackRowsAndWatermark(t *testing.T) {
	policy := Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 2}
	store, db, _ := testDebugStore(t, policy)
	h := NewHandler(store, HTTPOptions{Device: testAuthorizer{}})
	boot := "00000000000000000000000000000034"
	for _, seq := range []uint64{20, 21, 22} {
		upload := makeUpload("TEST-DEVICE", boot, "build", seq, 1)
		if w := postHTTPTransaction(t, h, upload); w.Code != http.StatusOK {
			t.Fatalf("seed seq=%d status=%d body=%s", seq, w.Code, w.Body)
		}
	}
	before := snapshotHTTPTransaction(t, db)
	if len(before.Watermarks) == 0 || len(before.Rows) != 2 {
		t.Fatalf("invalid nonempty prune baseline: %+v", before)
	}
	candidate := makeUpload("TEST-DEVICE", boot, "build", 23, 2)
	if _, err := DecodeUpload(bodyFor(t, candidate)); err != nil {
		t.Fatalf("candidate invalid: %v", err)
	}
	if _, err := db.Exec(`CREATE TRIGGER fail_watermark BEFORE INSERT ON device_debug_watermarks BEGIN SELECT RAISE(FAIL,'watermark-secret'); END`); err != nil {
		t.Fatal(err)
	}
	assertHTTPTransactionFailure(t, postHTTPTransaction(t, h, candidate), http.StatusInternalServerError)
	if after := snapshotHTTPTransaction(t, db); !reflect.DeepEqual(after, before) {
		t.Fatalf("prune state changed\nbefore=%+v\nafter=%+v", before, after)
	}
}
