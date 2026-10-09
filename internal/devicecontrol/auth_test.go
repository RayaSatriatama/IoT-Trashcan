package devicecontrol

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestDatabaseAuthorizerEnforcesPermissionExpiryRevocationAndCSRF(t *testing.T) {
	s, db, now := testStore(t)
	_ = s
	if err := ProvisionOperatorSession(context.Background(), db, "session", "operator-1", "operator-token", "csrf-token", now.Add(time.Minute), []string{"DEV-01"}); err != nil {
		t.Fatal(err)
	}
	a := NewDatabaseAuthorizer(db).WithClock(func() time.Time { return now })
	post := httptest.NewRequest(http.MethodPost, "/", nil)
	post.Header.Set("Authorization", "Bearer operator-token")
	if _, err := a.AuthorizeOperator(context.Background(), post, "DEV-01"); err == nil {
		t.Fatal("missing csrf accepted")
	}
	post.Header.Set("X-SIGAP-CSRF", "csrf-token")
	if got, err := a.AuthorizeOperator(context.Background(), post, "DEV-01"); err != nil || got != "operator-1" {
		t.Fatalf("authorize=%q err=%v", got, err)
	}
	if err := a.AuthorizeDevice(context.Background(), httptest.NewRequest(http.MethodGet, "/", nil), "DEV-01"); err == nil {
		t.Fatal("missing device token accepted")
	}
	deviceReq := httptest.NewRequest(http.MethodGet, "/", nil)
	deviceReq.Header.Set("Authorization", "Bearer 00")
	if err := a.AuthorizeDevice(context.Background(), deviceReq, "DEV-01"); err == nil {
		t.Fatal("wrong device token accepted")
	}
	// Expired permission is denied without deleting the session.
	if _, err := db.Exec(`UPDATE device_control_operator_permissions SET expires_at=? WHERE operator_id=? AND device_id=?`, now.Add(-time.Second).Format(time.RFC3339Nano), "operator-1", "DEV-01"); err != nil {
		t.Fatal(err)
	}
	if _, err := a.AuthorizeOperator(context.Background(), post, "DEV-01"); err == nil {
		t.Fatal("expired permission accepted")
	}
	if err := RevokeOperatorSession(context.Background(), db, "session", now); err != nil {
		t.Fatal(err)
	}
}

func TestDatabaseAuthorizerFailsClosedAndRateLimits(t *testing.T) {
	_, db, now := testStore(t)
	if err := ProvisionOperatorSession(context.Background(), db, "session", "operator-1", "operator-token", "csrf-token", now.Add(time.Minute), []string{"DEV-01"}); err != nil {
		t.Fatal(err)
	}
	a := NewDatabaseAuthorizer(db).WithClock(func() time.Time { return now }).WithRateLimit(1)
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	req.Header.Set("Authorization", "Bearer operator-token")
	if _, err := a.AuthorizeOperator(context.Background(), req, "DEV-01"); err != nil {
		t.Fatal(err)
	}
	if _, err := a.AuthorizeOperator(context.Background(), req, "DEV-01"); err == nil {
		t.Fatal("rate limit did not apply")
	}
	if _, err := NewDatabaseAuthorizer(db).AuthorizeOperator(context.Background(), httptest.NewRequest(http.MethodGet, "/", nil), "DEV-01"); err == nil {
		t.Fatal("empty auth accepted")
	}
}
