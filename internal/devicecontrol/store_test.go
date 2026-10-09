package devicecontrol

import (
	"context"
	"database/sql"
	"errors"
	"path/filepath"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
)

func testStore(t *testing.T) (*Store, *sql.DB, time.Time) {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "control.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES ('loc','test'); INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES ('dev','loc','DEV-01','00')`); err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)
	return NewStore(db).WithClock(func() time.Time { return now }), db, now
}

func commandRequest(now time.Time) CreateRequest {
	return CreateRequest{ID: "cmd-1", DeviceID: "DEV-01", OperatorID: "operator-1", SessionID: "session-1", SensorID: "hx711_organic", Operation: "start", Payload: []byte(`{"reference_mass_kg":2}`), ExpiresAt: now.Add(time.Minute)}
}

func TestEnqueueClaimReplayAndResultLifecycle(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	req := commandRequest(now)
	c, err := s.Enqueue(ctx, req)
	if err != nil {
		t.Fatal(err)
	}
	if c.Status != Queued {
		t.Fatalf("status %s", c.Status)
	}
	replay, err := s.Enqueue(ctx, req)
	if err != nil {
		t.Fatal(err)
	}
	if replay.RequestHash != c.RequestHash {
		t.Fatal("idempotent replay changed hash")
	}
	claimed, err := s.Claim(ctx, "DEV-01")
	if err != nil {
		t.Fatal(err)
	}
	if claimed.Status != Delivered || claimed.Attempt != 1 {
		t.Fatalf("claim=%+v", claimed)
	}
	for _, step := range []Status{Accepted, Capturing, Validated, PreviewReady, Committed} {
		if _, err := s.UpdateResult(ctx, "DEV-01", "cmd-1", ResultRequest{Status: step, SessionID: "session-1", Payload: []byte(`{"ok":true}`)}); err != nil {
			t.Fatalf("step %s: %v", step, err)
		}
	}
	if _, err := s.UpdateResult(ctx, "DEV-01", "cmd-1", ResultRequest{Status: Capturing}); !errors.Is(err, ErrConflict) {
		t.Fatalf("terminal replay err=%v", err)
	}
}

func TestHashConflictSessionBusyAndExpiry(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	req := commandRequest(now)
	if _, err := s.Enqueue(ctx, req); err != nil {
		t.Fatal(err)
	}
	conflict := req
	conflict.Payload = []byte(`{"reference_mass_kg":3}`)
	if _, err := s.Enqueue(ctx, conflict); !errors.Is(err, ErrConflict) {
		t.Fatalf("hash conflict err=%v", err)
	}
	busy := req
	busy.ID = "cmd-2"
	busy.OperatorID = "other"
	busy.SessionID = "other-session"
	if _, err := s.Enqueue(ctx, busy); !errors.Is(err, ErrSessionBusy) {
		t.Fatalf("session busy err=%v", err)
	}
	expired := req
	expired.ID = "cmd-expired"
	expired.SessionID = "session-1"
	expired.ExpiresAt = now.Add(-time.Second)
	if _, err := s.Enqueue(ctx, expired); !errors.Is(err, ErrExpired) {
		t.Fatalf("expiry err=%v", err)
	}
}

func TestUppercaseStartStillClaimsDeviceSession(t *testing.T) {
	s, _, now := testStore(t)
	req := commandRequest(now)
	req.ID = "cmd-uppercase"
	req.Operation = "START"
	if _, err := s.Enqueue(context.Background(), req); err != nil {
		t.Fatal(err)
	}
	other := req
	other.ID = "cmd-uppercase-other"
	other.OperatorID = "operator-2"
	other.SessionID = "session-2"
	if _, err := s.Enqueue(context.Background(), other); !errors.Is(err, ErrSessionBusy) {
		t.Fatalf("uppercase start did not reserve session: %v", err)
	}
}

func TestRestartKeepsQueueAndDeviceResultCannotCrossDevice(t *testing.T) {
	s, db, now := testStore(t)
	ctx := context.Background()
	req := commandRequest(now)
	if _, err := s.Enqueue(ctx, req); err != nil {
		t.Fatal(err)
	}
	// Recreate the store over the same database, representing a process restart.
	restarted := NewStore(db).WithClock(func() time.Time { return now })
	claimed, err := restarted.Claim(ctx, "DEV-01")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := restarted.UpdateResult(ctx, "OTHER", claimed.ID, ResultRequest{Status: Accepted}); !errors.Is(err, ErrNotFound) {
		t.Fatalf("cross-device result err=%v", err)
	}
	if _, err := restarted.UpdateResult(ctx, "DEV-01", claimed.ID, ResultRequest{Status: Accepted, SessionID: "session-1"}); err != nil {
		t.Fatal(err)
	}
}

func TestLatestResultReturnsExactDeviceEvidenceBody(t *testing.T) {
	s, _, now := testStore(t)
	req := commandRequest(now)
	req.ID = "cmd-diagnostic"
	req.SensorID = "sht31_44"
	if _, err := s.Enqueue(context.Background(), req); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Claim(context.Background(), req.DeviceID); err != nil {
		t.Fatal(err)
	}
	const payload = `{"sensor_id":"sht31_44","raw":{"present":true},"converted_value":null}`
	if _, err := s.UpdateResult(context.Background(), req.DeviceID, req.ID, ResultRequest{Status: Capturing, SessionID: req.SessionID, Payload: []byte(payload)}); err != nil {
		t.Fatal(err)
	}
	got, err := s.LatestResult(context.Background(), req.DeviceID, req.SensorID)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != payload {
		t.Fatalf("payload changed: %s", got)
	}
}
