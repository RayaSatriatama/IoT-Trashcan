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
	if _, err := s.UpdateResult(ctx, "DEV-01", "cmd-1", ResultRequest{
		Status: Accepted, SessionID: "session-1", Payload: []byte(`{"accepted":true}`),
	}); err != nil {
		t.Fatalf("start result: %v", err)
	}
	if _, err := s.UpdateResult(ctx, "DEV-01", "cmd-1", ResultRequest{Status: Capturing}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("per-command status err=%v", err)
	}
}

func TestClaimRequeuesExpiredLeaseWithoutChangingIdentity(t *testing.T) {
	now := time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)
	nowValue := now
	s, _, _ := testStore(t)
	s.WithLease(time.Second).WithClock(func() time.Time { return nowValue })
	ctx := context.Background()
	req := commandRequest(now)
	if _, err := s.Enqueue(ctx, req); err != nil {
		t.Fatal(err)
	}
	first, err := s.Claim(ctx, req.DeviceID)
	if err != nil {
		t.Fatal(err)
	}
	nowValue = now.Add(2 * time.Second)
	second, err := s.Claim(ctx, req.DeviceID)
	if err != nil {
		t.Fatal(err)
	}
	if second.ID != first.ID || second.Status != Delivered || second.Attempt != 2 {
		t.Fatalf("redelivery=%+v first=%+v", second, first)
	}
}

func TestTerminalResultReplayIsIdempotentAndConflictIsRejected(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	start := commandRequest(now)
	if _, err := s.Enqueue(ctx, start); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Claim(ctx, start.DeviceID); err != nil {
		t.Fatal(err)
	}
	if _, err := s.UpdateResult(ctx, start.DeviceID, start.ID, ResultRequest{Status: Accepted, SessionID: start.SessionID, Payload: []byte(`{"accepted":true}`)}); err != nil {
		t.Fatal(err)
	}
	commit := start
	commit.ID = "cmd-commit"
	commit.Operation = "commit"
	commit.Payload = []byte(`{"proposal_hash":"sha256:proposal"}`)
	if _, err := s.Enqueue(ctx, commit); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Claim(ctx, commit.DeviceID); err != nil {
		t.Fatal(err)
	}
	result := ResultRequest{Status: Committed, SessionID: "session-1", Payload: []byte(`{"profile_key":"hx711_organic","profile_hash":"sha256:fixture","version":1}`)}
	if _, err := s.UpdateResult(ctx, commit.DeviceID, commit.ID, result); err != nil {
		t.Fatal(err)
	}
	if _, err := s.UpdateResult(ctx, commit.DeviceID, commit.ID, result); err != nil {
		t.Fatalf("identical replay: %v", err)
	}
	result.Payload = []byte(`{"profile_key":"hx711_organic","profile_hash":"sha256:other","version":2}`)
	if _, err := s.UpdateResult(ctx, commit.DeviceID, commit.ID, result); !errors.Is(err, ErrConflict) {
		t.Fatalf("conflict=%v", err)
	}
	result.Payload = []byte(`{"profile_key":"hx711_organic","profile_hash":"sha256:fixture","version":1}`)
	result.ErrorCode = "CHANGED"
	if _, err := s.UpdateResult(ctx, commit.DeviceID, commit.ID, result); !errors.Is(err, ErrConflict) {
		t.Fatalf("error-code replay conflict=%v", err)
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
	if _, err := s.UpdateResult(context.Background(), req.DeviceID, req.ID, ResultRequest{Status: Accepted, SessionID: req.SessionID, Payload: []byte(payload)}); err != nil {
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

func TestCommitCommandCanReturnCommittedWithoutReplayingSessionHistory(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	start := commandRequest(now)
	if _, err := s.Enqueue(ctx, start); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Claim(ctx, start.DeviceID); err != nil {
		t.Fatal(err)
	}
	if _, err := s.UpdateResult(ctx, start.DeviceID, start.ID,
		ResultRequest{Status: Accepted, SessionID: start.SessionID, Payload: []byte(`{"accepted":true}`)}); err != nil {
		t.Fatal(err)
	}
	commit := start
	commit.ID = "cmd-commit"
	commit.Operation = "commit"
	commit.Payload = []byte(`{"proposal_hash":"sha256:proposal","expected_profile_version":"1"}`)
	if _, err := s.Enqueue(ctx, commit); err != nil {
		t.Fatalf("enqueue commit: %v", err)
	}
	if _, err := s.Claim(ctx, commit.DeviceID); err != nil {
		t.Fatal(err)
	}
	result := ResultRequest{Status: Committed, SessionID: commit.SessionID,
		Payload: []byte(`{"profile_key":"hx711_organic","profile_hash":"sha256:fixture","version":2}`)}
	if got, err := s.UpdateResult(ctx, commit.DeviceID, commit.ID, result); err != nil || got.Status != Committed {
		t.Fatalf("direct committed result=%+v err=%v", got, err)
	}
	// A lost response after the durable profile write is replayed exactly after
	// a backend restart, without creating another profile revision.
	restarted := NewStore(s.db).WithClock(func() time.Time { return now })
	if got, err := restarted.UpdateResult(ctx, commit.DeviceID, commit.ID, result); err != nil || got.ResultHash != hashPayload(result.Payload) {
		t.Fatalf("committed replay=%+v err=%v", got, err)
	}
}

func TestCommandIDReplayBindsEveryCommandIdentityField(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	req := commandRequest(now)
	if _, err := s.Enqueue(ctx, req); err != nil {
		t.Fatal(err)
	}
	for name, mutate := range map[string]func(*CreateRequest){
		"operator":  func(v *CreateRequest) { v.OperatorID = "operator-2" },
		"session":   func(v *CreateRequest) { v.SessionID = "session-2" },
		"sensor":    func(v *CreateRequest) { v.SensorID = "mq4" },
		"operation": func(v *CreateRequest) { v.Operation = "capture" },
		"expiry":    func(v *CreateRequest) { v.ExpiresAt = v.ExpiresAt.Add(time.Second) },
		"payload":   func(v *CreateRequest) { v.Payload = []byte(`{"reference_mass_kg":3}`) },
	} {
		t.Run(name, func(t *testing.T) {
			changed := req
			mutate(&changed)
			if _, err := s.Enqueue(ctx, changed); !errors.Is(err, ErrConflict) {
				t.Fatalf("changed %s replay err=%v", name, err)
			}
		})
	}
}

func TestMutatingCommandsRequireTheLiveOwnedSession(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	withoutStart := commandRequest(now)
	withoutStart.ID = "cmd-capture-without-session"
	withoutStart.Operation = "capture"
	withoutStart.SessionID = ""
	if _, err := s.Enqueue(ctx, withoutStart); !errors.Is(err, ErrInvalid) {
		t.Fatalf("missing session err=%v", err)
	}

	start := commandRequest(now)
	if _, err := s.Enqueue(ctx, start); err != nil {
		t.Fatal(err)
	}
	for name, mutate := range map[string]func(*CreateRequest){
		"operator": func(v *CreateRequest) { v.OperatorID = "operator-2" },
		"session":  func(v *CreateRequest) { v.SessionID = "session-2" },
		"sensor":   func(v *CreateRequest) { v.SensorID = "mq4" },
	} {
		t.Run(name, func(t *testing.T) {
			command := start
			command.ID = "cmd-" + name
			command.Operation = "capture"
			mutate(&command)
			if _, err := s.Enqueue(ctx, command); !errors.Is(err, ErrSessionBusy) {
				t.Fatalf("foreign session mutation err=%v", err)
			}
		})
	}
}

func TestCancelRequiresCommandOwner(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	req := commandRequest(now)
	if _, err := s.Enqueue(ctx, req); err != nil {
		t.Fatal(err)
	}
	if err := s.Cancel(ctx, req.DeviceID, req.ID, "operator-2"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("foreign cancel err=%v", err)
	}
	if err := s.Cancel(ctx, req.DeviceID, req.ID, req.OperatorID); err != nil {
		t.Fatalf("owner cancel err=%v", err)
	}
}
