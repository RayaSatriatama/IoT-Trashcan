package httpapi

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicedebug"
)

func TestDeviceDebugRouterUsesRealDatabaseAuthorization(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "router.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES('loc','test'); INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES('id','loc','TEST-DEVICE','')`); err != nil {
		t.Fatal(err)
	}
	token := "router-device-token"
	sum := sha256.Sum256([]byte(token))
	if _, err := db.Exec(`UPDATE devices SET credential_digest=? WHERE device_code=?`, hex.EncodeToString(sum[:]), "TEST-DEVICE"); err != nil {
		t.Fatal(err)
	}
	store, err := devicedebug.NewStore(db, devicedebug.DefaultPolicy())
	if err != nil {
		t.Fatal(err)
	}
	auth := devicecontrol.NewDatabaseAuthorizer(db).WithClock(func() time.Time { return time.Date(2026, 10, 22, 12, 0, 0, 0, time.UTC) })
	router := NewRouter(nil, WithDeviceDebug(store, devicedebug.HTTPOptions{Device: auth, Operator: auth}))
	body := []byte(`{"schema_version":1,"device_id":"TEST-DEVICE","boot_id":"00000000000000000000000000000001","build_id":"b","level":"INFO","captured_monotonic_us":"1","oldest_sequence":"1","overwritten":"0","delivery_lost":"0","events":[{"sequence":"1","monotonic_us":"1","level":"INFO","module":1,"event_code":1,"request_id":null,"duration_us":0,"arg0":0,"arg1":0,"related_sequence":null}]}`)
	req := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events", strings.NewReader(string(body)))
	req.Header.Set("Authorization", "Bearer "+token)
	rec := httptest.NewRecorder()
	router.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("router status=%d body=%s", rec.Code, rec.Body)
	}
}

func TestDeviceDebugRouterPreservesEscapedIdentifiers(t *testing.T) {
	for _, code := range []string{"A/B?C#D", "A%2FB+?", "TEST-ñ"} {
		t.Run(code, func(t *testing.T) {
			f := newAuthHTTPFixture(t)
			if _, err := f.db.Exec(`INSERT INTO devices(id,location_id,device_code,credential_digest) SELECT 'encoded-id','loc',?,credential_digest FROM devices WHERE device_code='TEST-DEVICE'`, code); err != nil {
				t.Fatal(err)
			}
			if _, err := f.db.Exec(`INSERT INTO device_control_operator_permissions(operator_id,device_id) VALUES('operator',?)`, code); err != nil {
				t.Fatal(err)
			}
			body := []byte(strings.ReplaceAll(string(f.upload), "TEST-DEVICE", code))
			path := "/api/device-debug/device/" + url.PathEscape(code) + "/events"
			posted := f.request(http.MethodPost, path, "device-token", body)
			if posted.Code != http.StatusOK {
				t.Fatalf("encoded upload status=%d body=%s", posted.Code, posted.Body)
			}
			var ack devicedebug.UploadAck
			if err := json.Unmarshal(posted.Body.Bytes(), &ack); err != nil {
				t.Fatal(err)
			}
			if ack.DeviceID != code || ack.Status != "DURABLE" {
				t.Fatalf("ACK lost identifier: %+v", ack)
			}
			if got := f.request(http.MethodPost, path, "wrong-token", body); got.Code != http.StatusUnauthorized {
				t.Fatalf("encoded route bypassed authorization: %d", got.Code)
			}
			if got := f.request(http.MethodPost, path, "device-token", f.upload); got.Code != http.StatusBadRequest {
				t.Fatalf("encoded route accepted mismatched body identity: %d", got.Code)
			}
			page := f.request(http.MethodGet, "/api/device-debug/events/"+url.PathEscape(code), "operator-token", nil)
			if page.Code != http.StatusOK {
				t.Fatalf("encoded history status=%d body=%s", page.Code, page.Body)
			}
			var history devicedebug.HistoryPage
			if err := json.Unmarshal(page.Body.Bytes(), &history); err != nil {
				t.Fatal(err)
			}
			if history.DeviceID != code || len(history.Events) != 1 {
				t.Fatalf("history identity/count changed: %+v", history)
			}
		})
	}
}

func TestSharedDatabaseAuthorizerQuotaControlToDebug(t *testing.T) { testSharedQuota(t, true) }
func TestSharedDatabaseAuthorizerQuotaDebugToControl(t *testing.T) { testSharedQuota(t, false) }
func testSharedQuota(t *testing.T, controlFirst bool) {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "quota.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES('loc','test'); INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES('id','loc','TEST-DEVICE','')`); err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 10, 22, 12, 0, 0, 0, time.UTC)
	if err := devicecontrol.ProvisionOperatorSession(t.Context(), db, "session", "operator", "operator-token", "csrf", now.Add(time.Hour), []string{"TEST-DEVICE"}); err != nil {
		t.Fatal(err)
	}
	auth := devicecontrol.NewDatabaseAuthorizer(db).WithClock(func() time.Time { return now }).WithRateLimit(1)
	debugStore, err := devicedebug.NewStore(db, devicedebug.DefaultPolicy())
	if err != nil {
		t.Fatal(err)
	}
	controlStore := devicecontrol.NewStore(db)
	router := NewRouter(nil, WithDeviceDebug(debugStore, devicedebug.HTTPOptions{Operator: auth}), WithDeviceControl(controlStore, devicecontrol.HTTPOptions{Operator: auth, Device: auth, Diagnostics: controlStore}))
	paths := []string{"/api/device-control/commands/TEST-DEVICE/capabilities?sensor_id=hx711_organic", "/api/device-debug/events/TEST-DEVICE"}
	if !controlFirst {
		paths[0], paths[1] = paths[1], paths[0]
	}
	for i, path := range paths {
		r := httptest.NewRequest(http.MethodGet, path, nil)
		r.Header.Set("Authorization", "Bearer operator-token")
		w := httptest.NewRecorder()
		router.ServeHTTP(w, r)
		if i == 0 && w.Code != http.StatusOK {
			t.Fatalf("first shared quota route %s status=%d body=%s", path, w.Code, w.Body)
		}
		if i == 1 && w.Code != http.StatusUnauthorized {
			t.Fatalf("second shared quota route %s status=%d body=%s", path, w.Code, w.Body)
		}
	}
}

func TestRealDatabaseAuthorizationHTTPMatrix(t *testing.T) {
	cases := []struct {
		name, token, path, method string
		change                    func(*testing.T, *sql.DB, time.Time)
	}{
		{name: "missing_device_token", method: http.MethodPost, path: "/api/device-debug/device/TEST-DEVICE/events"},
		{name: "wrong_device_token", method: http.MethodPost, path: "/api/device-debug/device/TEST-DEVICE/events", token: "wrong-token"},
		{name: "operator_token_on_device_route", method: http.MethodPost, path: "/api/device-debug/device/TEST-DEVICE/events", token: "operator-token"},
		{name: "device_token_on_operator_route", method: http.MethodGet, path: "/api/device-debug/events/TEST-DEVICE", token: "device-token"},
		{name: "missing_operator_token", method: http.MethodGet, path: "/api/device-debug/events/TEST-DEVICE"},
		{name: "wrong_operator_token", method: http.MethodGet, path: "/api/device-debug/events/TEST-DEVICE", token: "wrong-token"},
		{name: "device_token_for_other_device", method: http.MethodPost, path: "/api/device-debug/device/OTHER-DEVICE/events", token: "device-token"},
		{name: "operator_without_other_device_permission", method: http.MethodGet, path: "/api/device-debug/events/OTHER-DEVICE", token: "operator-token"},
		{name: "expired_operator_session", method: http.MethodGet, path: "/api/device-debug/events/TEST-DEVICE", token: "operator-token", change: func(t *testing.T, db *sql.DB, now time.Time) {
			if _, err := db.Exec(`UPDATE device_control_operator_sessions SET expires_at=? WHERE session_id='session'`, now.Format(time.RFC3339Nano)); err != nil {
				t.Fatal(err)
			}
		}},
		{name: "revoked_operator_session", method: http.MethodGet, path: "/api/device-debug/events/TEST-DEVICE", token: "operator-token", change: func(t *testing.T, db *sql.DB, now time.Time) {
			if err := devicecontrol.RevokeOperatorSession(t.Context(), db, "session", now); err != nil {
				t.Fatal(err)
			}
		}},
		{name: "expired_operator_permission", method: http.MethodGet, path: "/api/device-debug/events/TEST-DEVICE", token: "operator-token", change: func(t *testing.T, db *sql.DB, now time.Time) {
			if _, err := db.Exec(`UPDATE device_control_operator_permissions SET expires_at=? WHERE operator_id='operator' AND device_id='TEST-DEVICE'`, now.Format(time.RFC3339Nano)); err != nil {
				t.Fatal(err)
			}
		}},
		{name: "revoked_operator_permission", method: http.MethodGet, path: "/api/device-debug/events/TEST-DEVICE", token: "operator-token", change: func(t *testing.T, db *sql.DB, now time.Time) {
			if _, err := db.Exec(`UPDATE device_control_operator_permissions SET revoked_at=? WHERE operator_id='operator' AND device_id='TEST-DEVICE'`, now.Format(time.RFC3339Nano)); err != nil {
				t.Fatal(err)
			}
		}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			f := newAuthHTTPFixture(t)
			if tc.change != nil {
				validToken := "operator-token"
				if tc.method == http.MethodPost {
					validToken = "device-token"
				}
				before := f.request(tc.method, tc.path, validToken, f.upload)
				if before.Code != http.StatusOK {
					t.Fatalf("positive baseline status=%d body=%s", before.Code, before.Body)
				}
				tc.change(t, f.db, f.now)
			}
			got := f.request(tc.method, tc.path, tc.token, f.upload)
			if got.Code != http.StatusUnauthorized {
				t.Fatalf("status=%d body=%s", got.Code, got.Body)
			}
			if strings.TrimSpace(got.Body.String()) != `{"error":"unauthorized"}` {
				t.Fatalf("non-generic auth error: %s", got.Body)
			}
			if (tc.token != "" && strings.Contains(got.Body.String(), tc.token)) || strings.Contains(got.Body.String(), "session") || strings.Contains(got.Body.String(), "permission") {
				t.Fatalf("authorization details leaked: %s", got.Body)
			}
			var rows int
			if err := f.db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&rows); err != nil {
				t.Fatal(err)
			}
			if rows != 0 {
				t.Fatalf("unauthorized request changed debug rows: %d", rows)
			}
		})
	}
	t.Run("positive_device_upload_and_operator_history", func(t *testing.T) {
		f := newAuthHTTPFixture(t)
		if got := f.request(http.MethodPost, "/api/device-debug/device/TEST-DEVICE/events", "device-token", f.upload); got.Code != http.StatusOK {
			t.Fatalf("device upload status=%d body=%s", got.Code, got.Body)
		}
		if got := f.request(http.MethodGet, "/api/device-debug/events/TEST-DEVICE", "operator-token", nil); got.Code != http.StatusOK {
			t.Fatalf("operator history status=%d body=%s", got.Code, got.Body)
		}
	})
}

func TestDeviceCredentialSharedQuotaControlAndDebugBothDirections(t *testing.T) {
	for _, debugFirst := range []bool{false, true} {
		name := "control_to_debug"
		if debugFirst {
			name = "debug_to_control"
		}
		t.Run(name, func(t *testing.T) {
			f := newAuthHTTPFixture(t)
			auth := devicecontrol.NewDatabaseAuthorizer(f.db).WithClock(func() time.Time { return f.now }).WithRateLimit(1)
			debugStore, err := devicedebug.NewStore(f.db, devicedebug.DefaultPolicy())
			if err != nil {
				t.Fatal(err)
			}
			controlStore := devicecontrol.NewStore(f.db)
			router := NewRouter(nil, WithDeviceDebug(debugStore, devicedebug.HTTPOptions{Device: auth, Operator: auth}), WithDeviceControl(controlStore, devicecontrol.HTTPOptions{Device: auth, Operator: auth}))
			controlPath, debugPath := "/api/device-control/device/TEST-DEVICE/next", "/api/device-debug/device/TEST-DEVICE/events"
			first, second, firstMethod := controlPath, debugPath, http.MethodGet
			if debugFirst {
				first, second, firstMethod = debugPath, controlPath, http.MethodPost
			}
			run := func(path, method string) *httptest.ResponseRecorder {
				r := httptest.NewRequest(method, path, strings.NewReader(string(f.upload)))
				r.Header.Set("Authorization", "Bearer device-token")
				w := httptest.NewRecorder()
				router.ServeHTTP(w, r)
				return w
			}
			firstResult := run(first, firstMethod)
			wantFirst := http.StatusNoContent
			if debugFirst {
				wantFirst = http.StatusOK
			}
			if firstResult.Code != wantFirst {
				t.Fatalf("first route %s status=%d want=%d body=%s", first, firstResult.Code, wantFirst, firstResult.Body)
			}
			secondMethod := http.MethodGet
			if !debugFirst {
				secondMethod = http.MethodPost
			}
			secondResult := run(second, secondMethod)
			if secondResult.Code != http.StatusUnauthorized {
				t.Fatalf("second shared-quota request status=%d body=%s", secondResult.Code, secondResult.Body)
			}
			var rows int
			if err := f.db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&rows); err != nil {
				t.Fatal(err)
			}
			wantRows := 0
			if debugFirst {
				wantRows = 1
			}
			if rows != wantRows {
				t.Fatalf("debug rows after shared-quota sequence=%d want=%d", rows, wantRows)
			}
		})
	}
}

type authHTTPFixture struct {
	db     *sql.DB
	router http.Handler
	upload []byte
	now    time.Time
}

func newAuthHTTPFixture(t *testing.T) *authHTTPFixture {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "auth.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES('loc','test'); INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES('id1','loc','TEST-DEVICE',''),('id2','loc','OTHER-DEVICE','')`); err != nil {
		t.Fatal(err)
	}
	deviceHash := sha256.Sum256([]byte("device-token"))
	if _, err := db.Exec(`UPDATE devices SET credential_digest=? WHERE device_code='TEST-DEVICE'`, hex.EncodeToString(deviceHash[:])); err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 10, 22, 12, 0, 0, 0, time.UTC)
	if err := devicecontrol.ProvisionOperatorSession(t.Context(), db, "session", "operator", "operator-token", "csrf-token", now.Add(time.Hour), []string{"TEST-DEVICE"}); err != nil {
		t.Fatal(err)
	}
	auth := devicecontrol.NewDatabaseAuthorizer(db).WithClock(func() time.Time { return now })
	debugStore, err := devicedebug.NewStore(db, devicedebug.DefaultPolicy())
	if err != nil {
		t.Fatal(err)
	}
	controlStore := devicecontrol.NewStore(db)
	router := NewRouter(nil, WithDeviceDebug(debugStore, devicedebug.HTTPOptions{Device: auth, Operator: auth}), WithDeviceControl(controlStore, devicecontrol.HTTPOptions{Device: auth, Operator: auth}))
	upload := []byte(`{"schema_version":1,"device_id":"TEST-DEVICE","boot_id":"00000000000000000000000000000001","build_id":"b","level":"INFO","captured_monotonic_us":"1","oldest_sequence":"1","overwritten":"0","delivery_lost":"0","events":[{"sequence":"1","monotonic_us":"1","level":"INFO","module":1,"event_code":1,"request_id":null,"duration_us":0,"arg0":0,"arg1":0,"related_sequence":null}]}`)
	return &authHTTPFixture{db: db, router: router, upload: upload, now: now}
}

func (f *authHTTPFixture) request(method, path, token string, body []byte) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, strings.NewReader(string(body)))
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	f.router.ServeHTTP(w, r)
	return w
}
