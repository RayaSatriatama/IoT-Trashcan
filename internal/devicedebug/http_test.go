package devicedebug

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
)

type testAuthorizer struct {
	operator string
	err      error
}

func (a testAuthorizer) AuthorizeOperator(context.Context, *http.Request, string) (string, error) {
	return a.operator, a.err
}
func (a testAuthorizer) AuthorizeDevice(context.Context, *http.Request, string) error { return a.err }

func TestHTTPHandlerPreservesEscapedIdentifiers(t *testing.T) {
	for _, code := range []string{"A/B?C#D", "A%2FB+?", "TEST-ñ"} {
		t.Run(code, func(t *testing.T) {
			store, db, _ := testDebugStore(t, DefaultPolicy())
			if _, err := db.Exec(`INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES('encoded-id','loc',?,'test-only-digest')`, code); err != nil {
				t.Fatal(err)
			}
			upload, _ := fixtureUpload(t)
			upload.DeviceID = code
			auth := testAuthorizer{operator: "operator"}
			handler := NewHandler(store, HTTPOptions{Device: auth, Operator: auth})
			posted := httptest.NewRecorder()
			handler.ServeHTTP(posted, httptest.NewRequest(http.MethodPost, "/api/device-debug/device/"+url.PathEscape(code)+"/events", strings.NewReader(string(bodyFor(t, upload)))))
			if posted.Code != http.StatusOK {
				t.Fatalf("direct encoded upload status=%d body=%s", posted.Code, posted.Body)
			}
			page := httptest.NewRecorder()
			handler.ServeHTTP(page, httptest.NewRequest(http.MethodGet, "/api/device-debug/events/"+url.PathEscape(code), nil))
			if page.Code != http.StatusOK {
				t.Fatalf("direct encoded history status=%d body=%s", page.Code, page.Body)
			}
			var history HistoryPage
			if err := json.Unmarshal(page.Body.Bytes(), &history); err != nil {
				t.Fatal(err)
			}
			if history.DeviceID != code || len(history.Events) != len(upload.Events) {
				t.Fatalf("direct handler changed identity/count: %+v", history)
			}
		})
	}
}

func TestHTTPUploadHashesExactWhitespaceBytesAndHistoryDefaults(t *testing.T) {
	store, db, now := testDebugStore(t, DefaultPolicy())
	_, raw := fixtureUpload(t)
	body := append([]byte(" \n"), raw...)
	body = append(body, '\n', ' ')
	deviceToken := "test-device-token"
	deviceSum := sha256.Sum256([]byte(deviceToken))
	if _, err := db.Exec(`UPDATE devices SET credential_digest=? WHERE device_code=?`, hex.EncodeToString(deviceSum[:]), "TEST-DEVICE"); err != nil {
		t.Fatal(err)
	}
	if err := devicecontrol.ProvisionOperatorSession(context.Background(), db, "session", "operator", "operator-token", "csrf", now.Add(time.Hour), []string{"TEST-DEVICE"}); err != nil {
		t.Fatal(err)
	}
	auth := devicecontrol.NewDatabaseAuthorizer(db).WithClock(func() time.Time { return *now })
	handler := NewHandler(store, HTTPOptions{Operator: auth, Device: auth, AllowedOrigin: "https://operator.example"})
	req := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events", strings.NewReader(string(body)))
	req.Header.Set("Authorization", "Bearer "+deviceToken)
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("POST status=%d body=%s", rec.Code, rec.Body)
	}
	var ack UploadAck
	if err := json.Unmarshal(rec.Body.Bytes(), &ack); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	if ack.Status != "DURABLE" || ack.RequestSHA256 != hex.EncodeToString(sum[:]) {
		t.Fatalf("ACK=%+v", ack)
	}
	get := httptest.NewRequest(http.MethodGet, "/api/device-debug/events/TEST-DEVICE", nil)
	get.Header.Set("Authorization", "Bearer operator-token")
	page := httptest.NewRecorder()
	handler.ServeHTTP(page, get)
	if page.Code != http.StatusOK {
		t.Fatalf("GET status=%d body=%s", page.Code, page.Body)
	}
	var history HistoryPage
	if err := json.Unmarshal(page.Body.Bytes(), &history); err != nil {
		t.Fatal(err)
	}
	if len(history.Events) != 1 || history.Events[0].IngestionID == "" || history.NextAfter != history.Events[0].IngestionID {
		t.Fatalf("history=%+v", history)
	}
}

func TestMissingDebugAuthorizersFailClosed(t *testing.T) {
	store, _, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{})
	for _, tc := range []struct{ method, path string }{{http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events"}, {http.MethodGet, "/api/device-debug/events/TEST-DEVICE"}} {
		r := httptest.NewRequest(tc.method, tc.path, strings.NewReader("{}"))
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != http.StatusUnauthorized {
			t.Fatalf("%s %s status=%d", tc.method, tc.path, w.Code)
		}
	}
}

func TestHTTPHistoryRejectsEmptyOperatorIdentity(t *testing.T) {
	store, _, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Operator: testAuthorizer{}})
	r := httptest.NewRequest(http.MethodGet, "/api/device-debug/events/TEST-DEVICE", nil)
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("status=%d body=%s", w.Code, w.Body)
	}
}

func TestHTTPHistoryRejectsMalformedAndNoncanonicalQueries(t *testing.T) {
	store, _, _ := testDebugStore(t, DefaultPolicy())
	auth := testAuthorizer{operator: "operator"}
	h := NewHandler(store, HTTPOptions{Operator: auth})
	for _, query := range []string{"?after=%zz", "?after=1;limit=2", "?limit=+1", "?limit=01", "?after=", "?limit=", "?limit=1&limit=2", "?unknown=1"} {
		t.Run(query, func(t *testing.T) {
			r := httptest.NewRequest(http.MethodGet, "/api/device-debug/events/TEST-DEVICE"+query, nil)
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if w.Code != http.StatusBadRequest {
				t.Fatalf("query %q status=%d body=%s", query, w.Code, w.Body)
			}
		})
	}
}

func TestHTTPUploadOversizeKnownAndUnknownLengthDoesNotMutate(t *testing.T) {
	store, db, _ := testDebugStore(t, DefaultPolicy())
	auth := testAuthorizer{}
	h := NewHandler(store, HTTPOptions{Device: auth})
	for _, unknown := range []bool{false, true} {
		body := strings.Repeat("x", httpMaxBody+1)
		r := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events", strings.NewReader(body))
		if unknown {
			r.ContentLength = -1
		}
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != http.StatusRequestEntityTooLarge {
			t.Fatalf("unknown=%v status=%d", unknown, w.Code)
		}
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 0 {
		t.Fatalf("oversize request mutated history: %d", count)
	}
}

func TestHTTPWrongOriginIsForbidden(t *testing.T) {
	store, _, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Operator: testAuthorizer{operator: "operator"}, AllowedOrigin: "https://allowed.example"})
	r := httptest.NewRequest(http.MethodGet, "/api/device-debug/events/TEST-DEVICE", nil)
	r.Header.Set("Origin", "https://wrong.example")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != http.StatusForbidden {
		t.Fatalf("status=%d", w.Code)
	}
}

func TestHTTPUploadRejectsMalformedAndPathMismatchWithoutMutation(t *testing.T) {
	store, db, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Device: testAuthorizer{}})
	_, raw := fixtureUpload(t)
	cases := []struct {
		name, path string
		body       []byte
	}{
		{"path_identity", "/api/device-debug/device/OTHER/events", raw},
		{"duplicate_key", "/api/device-debug/device/TEST-DEVICE/events", []byte(strings.Replace(string(raw), `"schema_version":1`, `"schema_version":1,"schema_version":1`, 1))},
		{"unknown_key", "/api/device-debug/device/TEST-DEVICE/events", []byte(strings.Replace(string(raw), `"events":[`, `"extra":1,"events":[`, 1))},
		{"trailing", "/api/device-debug/device/TEST-DEVICE/events", append(append([]byte(nil), raw...), []byte(` {}`)...)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			r := httptest.NewRequest(http.MethodPost, tc.path, strings.NewReader(string(tc.body)))
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if w.Code != http.StatusBadRequest {
				t.Fatalf("status=%d body=%s", w.Code, w.Body)
			}
		})
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 0 {
		t.Fatalf("invalid request wrote %d rows", count)
	}
}

func TestHTTPUploadRetryConflictAndStorageFailureNeverClaimsDurable(t *testing.T) {
	store, db, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Device: testAuthorizer{}})
	_, raw := fixtureUpload(t)
	post := func(body []byte) *httptest.ResponseRecorder {
		r := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events", strings.NewReader(string(body)))
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w
	}
	if w := post(raw); w.Code != http.StatusOK {
		t.Fatalf("first status=%d body=%s", w.Code, w.Body)
	}
	second := post(raw)
	if second.Code != http.StatusOK {
		t.Fatalf("retry status=%d body=%s", second.Code, second.Body)
	}
	var ack UploadAck
	if err := json.Unmarshal(second.Body.Bytes(), &ack); err != nil {
		t.Fatal(err)
	}
	if ack.StoredCount != 0 || ack.DeduplicatedCount != 1 {
		t.Fatalf("retry ack=%+v", ack)
	}
	conflict := []byte(strings.Replace(string(raw), `"build_id":"TEST"`, `"build_id":"OTHER"`, 1))
	if w := post(conflict); w.Code != http.StatusConflict {
		t.Fatalf("conflict status=%d body=%s", w.Code, w.Body)
	}
	if _, err := db.Exec(`CREATE TRIGGER reject_debug_history BEFORE INSERT ON device_debug_history BEGIN SELECT RAISE(ABORT,'private trigger detail'); END`); err != nil {
		t.Fatal(err)
	}
	changed := []byte(strings.Replace(string(raw), `"sequence":"2"`, `"sequence":"3"`, 1))
	w := post(changed)
	if w.Code != http.StatusInternalServerError || strings.Contains(w.Body.String(), "private trigger detail") || strings.Contains(w.Body.String(), "DURABLE") {
		t.Fatalf("storage response status=%d body=%s", w.Code, w.Body)
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("storage failure persisted rows: %d", count)
	}
}

func TestHTTPExactBodyUTF8AndQueryCursorBoundaries(t *testing.T) {
	store, db, _ := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Device: testAuthorizer{}, Operator: testAuthorizer{operator: "operator"}})
	_, raw := fixtureUpload(t)
	body := append([]byte(nil), raw...)
	body = append(body, strings.Repeat(" ", httpMaxBody-len(body))...)
	if len(body) != httpMaxBody {
		t.Fatalf("body bytes=%d", len(body))
	}
	post := func(b []byte, length int64) *httptest.ResponseRecorder {
		r := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events", strings.NewReader(string(b)))
		r.ContentLength = length
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w
	}
	w := post(body, int64(len(body)))
	if w.Code != http.StatusOK {
		t.Fatalf("exact limit status=%d %s", w.Code, w.Body)
	}
	var ack UploadAck
	if err := json.Unmarshal(w.Body.Bytes(), &ack); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	if ack.RequestSHA256 != hex.EncodeToString(sum[:]) {
		t.Fatalf("hash=%s", ack.RequestSHA256)
	}
	if w = post(append(append([]byte(nil), body...), ' '), int64(httpMaxBody+1)); w.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversize status=%d", w.Code)
	}
	bad := append(append([]byte(nil), raw...), 0xff)
	if w = post(bad, int64(len(bad))); w.Code != http.StatusBadRequest {
		t.Fatalf("invalid UTF8 status=%d", w.Code)
	}
	for _, q := range []string{"?after=-1", "?after=+1", "?after=01", "?after=9223372036854775808", "?limit=0", "?limit=101"} {
		r := httptest.NewRequest(http.MethodGet, "/api/device-debug/events/TEST-DEVICE"+q, nil)
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != http.StatusBadRequest {
			t.Errorf("%s status=%d", q, w.Code)
		}
	}
	var rows int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&rows); err != nil || rows != 1 {
		t.Fatalf("rows=%d err=%v", rows, err)
	}
}

func TestHTTPHistoryPaginationKeepsDuplicateReceiptAndDeviceIsolation(t *testing.T) {
	store, db, now := testDebugStore(t, DefaultPolicy())
	h := NewHandler(store, HTTPOptions{Device: testAuthorizer{}, Operator: testAuthorizer{operator: "operator"}})
	post := func(u Upload) *httptest.ResponseRecorder {
		b := bodyFor(t, u)
		r := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/"+u.DeviceID+"/events", strings.NewReader(string(b)))
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w
	}
	u := makeUpload("TEST-DEVICE", "00000000000000000000000000000001", "build-a", 9007199254740993, 2)
	u.Events[0].Sequence = "9007199254740993"
	u.Events[0].MonotonicUS = "9007199254740993"
	u.Events[0].EventCode = 1
	u.Events[0].Module = 1
	u.Events[1].Sequence = "18446744073709551615"
	u.Events[1].MonotonicUS = "18446744073709551615"
	u.Events[1].EventCode = 1
	u.Events[1].Module = 1
	u.CapturedMonotonicUS = "18446744073709551615"
	u.OldestSequence = "9007199254740993"
	if w := post(u); w.Code != 200 {
		t.Fatalf("POST=%d %s", w.Code, w.Body)
	}
	var receipt string
	if err := db.QueryRow(`SELECT received_at FROM device_debug_history ORDER BY ingestion_id LIMIT 1`).Scan(&receipt); err != nil {
		t.Fatal(err)
	}
	*now = now.Add(time.Hour)
	if w := post(u); w.Code != 200 {
		t.Fatalf("retry=%d", w.Code)
	}
	get := func(path string) HistoryPage {
		r := httptest.NewRequest(http.MethodGet, path, nil)
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != 200 {
			t.Fatalf("GET=%d %s", w.Code, w.Body)
		}
		var p HistoryPage
		if err := json.Unmarshal(w.Body.Bytes(), &p); err != nil {
			t.Fatal(err)
		}
		return p
	}
	p := get("/api/device-debug/events/TEST-DEVICE?limit=1")
	if len(p.Events) != 1 || !p.HasMore || p.NextAfter == "" {
		t.Fatalf("page=%+v", p)
	}
	q := get("/api/device-debug/events/TEST-DEVICE?after=" + p.NextAfter + "&limit=1")
	if len(q.Events) != 1 || q.HasMore {
		t.Fatalf("page2=%+v", q)
	}
	if p.Events[0].Sequence != "9007199254740993" || p.Events[0].ReceivedAt != receipt {
		t.Fatalf("event=%+v receipt=%s", p.Events[0], receipt)
	}
	if p := get("/api/device-debug/events/OTHER-DEVICE"); len(p.Events) != 0 {
		t.Fatalf("cross-device leak=%+v", p)
	}
}
