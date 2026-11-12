package devicecontrol

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strconv"
	"sync"
	"sync/atomic"
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

func TestDatabaseAuthorizerBoundsInvalidTokenStateWithoutEvictingExistingQuota(t *testing.T) {
	const capacity = 4096
	_, db, now := testStore(t)
	if err := ProvisionOperatorSession(t.Context(), db, "bounded-session", "operator-1", "bounded-operator-token", "csrf-token", now.Add(time.Hour), []string{"DEV-01"}); err != nil {
		t.Fatal(err)
	}
	a := NewDatabaseAuthorizer(db).WithClock(func() time.Time { return now }).WithRateLimit(2)
	valid := httptest.NewRequest(http.MethodGet, "/", nil)
	valid.Header.Set("Authorization", "Bearer bounded-operator-token")
	if _, err := a.AuthorizeOperator(t.Context(), valid, "DEV-01"); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < capacity+8; i++ {
		req := httptest.NewRequest(http.MethodGet, "/", nil)
		req.Header.Set("Authorization", "Bearer TEST_ONLY_INVALID_TOKEN_"+strconv.Itoa(i))
		if i%2 == 0 {
			if _, err := a.AuthorizeOperator(t.Context(), req, "DEV-01"); err == nil {
				t.Fatal("invalid operator token accepted")
			}
		} else if err := a.AuthorizeDevice(t.Context(), req, "DEV-01"); err == nil {
			t.Fatal("invalid device token accepted")
		}
	}
	if len(a.requests) != capacity {
		t.Fatalf("tracked token keys=%d, want bounded capacity=%d", len(a.requests), capacity)
	}
	if _, err := a.AuthorizeOperator(t.Context(), valid, "DEV-01"); err != nil {
		t.Fatalf("capacity rejected an existing key below quota: %v", err)
	}
	if _, err := a.AuthorizeOperator(t.Context(), valid, "DEV-01"); err == nil {
		t.Fatal("capacity handling reset an existing quota")
	}
}

func TestDatabaseAuthorizerRateKeyCapacityResetsOnlyAtMinuteRollover(t *testing.T) {
	const capacity = 4096
	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	a := NewDatabaseAuthorizer(nil).WithRateLimit(1)
	for i := 0; i < capacity; i++ {
		if !a.allowRate("key-"+strconv.Itoa(i), now) {
			t.Fatal("new key rejected below capacity")
		}
	}
	if a.allowRate("overflow", now) || len(a.requests) != capacity {
		t.Fatal("full window admitted an untracked key")
	}
	next := now.Add(time.Minute)
	if !a.allowRate("next-window", next) || len(a.requests) != 1 {
		t.Fatal("minute rollover did not release the old bounded table")
	}
	if !a.allowRate("key-0", next) || a.allowRate("key-0", next) {
		t.Fatal("per-key quota changed after minute rollover")
	}
}

func TestDatabaseAuthorizerRateKeyCapacityIsAtomic(t *testing.T) {
	const capacity = 4096
	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	a := NewDatabaseAuthorizer(nil)
	var workers sync.WaitGroup
	var admitted atomic.Int64
	start := make(chan struct{})
	for worker := 0; worker < 32; worker++ {
		workers.Add(1)
		go func(worker int) {
			defer workers.Done()
			<-start
			for i := worker; i < capacity*2; i += 32 {
				if a.allowRate("concurrent-key-"+strconv.Itoa(i), now) {
					admitted.Add(1)
				}
			}
		}(worker)
	}
	close(start)
	workers.Wait()
	if admitted.Load() != capacity || len(a.requests) != capacity {
		t.Fatalf("capacity race: admitted=%d tracked=%d", admitted.Load(), len(a.requests))
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
