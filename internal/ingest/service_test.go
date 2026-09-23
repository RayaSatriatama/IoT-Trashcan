package ingest

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"

	_ "modernc.org/sqlite"
)

func TestIngestPersistsThreeReadingsAndHeartbeat(t *testing.T) {
	db, service := testService(t)
	defer db.Close()

	service.now = func() time.Time { return time.Date(2026, 9, 16, 12, 0, 0, 0, time.UTC) }
	result, err := service.Ingest(context.Background(), "fixture-token", validRequest(), []byte(`{"device_id":"SIGAP-01","health":{"wifi_rssi":-61}}`))
	if err != nil {
		t.Fatalf("Ingest() error = %v", err)
	}
	if result.ReadingsStored != 3 || result.DeviceID != "SIGAP-01" {
		t.Fatalf("Ingest() result = %#v", result)
	}

	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM readings`).Scan(&count); err != nil || count != 3 {
		t.Fatalf("reading count = %d, err = %v", count, err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM decisions`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("decision count = %d, err = %v; unconfigured runtime must not fabricate decisions", count, err)
	}
	var lastSeen string
	if err := db.QueryRow(`SELECT last_seen FROM devices WHERE device_code = 'SIGAP-01'`).Scan(&lastSeen); err != nil {
		t.Fatal(err)
	}
	if lastSeen != "2026-09-16T12:00:00Z" {
		t.Fatalf("last_seen = %q", lastSeen)
	}
}

func TestIngestRejectsBadCredentialsWithoutWrites(t *testing.T) {
	db, service := testService(t)
	defer db.Close()

	_, err := service.Ingest(context.Background(), "wrong-token", validRequest(), []byte(`{}`))
	if !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("Ingest() error = %v, want unauthorized", err)
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM readings`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("reading count = %d, err = %v", count, err)
	}
}

func TestIngestRejectsMalformedSlotsBeforeWrites(t *testing.T) {
	db, service := testService(t)
	defer db.Close()

	req := validRequest()
	req.Slots[2].Stream = "organic"
	_, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{}`))
	if !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("Ingest() error = %v, want invalid request", err)
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM readings`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("reading count = %d, err = %v", count, err)
	}
}

func TestIngestRejectsValidTokenForAnotherClaimedDeviceWithoutWrites(t *testing.T) {
	db, service := testService(t)
	defer db.Close()

	req := validRequest()
	req.DeviceID = "SIGAP-02"
	_, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{}`))
	if !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("Ingest() error = %v, want unauthorized", err)
	}
	assertReadingsForDevice(t, db, "device-2", 0)
}

func TestIngestRejectsMissingStreamWithoutWrites(t *testing.T) {
	db, service := testService(t)
	defer db.Close()

	req := validRequest()
	req.Slots = []Slot{req.Slots[0], req.Slots[2]}
	_, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{}`))
	if !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("Ingest() error = %v, want invalid request", err)
	}
	assertReadingsForDevice(t, db, "device-1", 0)
}

func TestIngestRollsBackReadingsAndHeartbeatWhenReadingInsertFails(t *testing.T) {
	db, service := testService(t)
	defer db.Close()

	if _, err := db.Exec(`
		CREATE TRIGGER fail_b3_insert
		BEFORE INSERT ON readings
		WHEN NEW.slot_id = 'slot-b3'
		BEGIN
			SELECT RAISE(ABORT, 'forced test failure');
		END`); err != nil {
		t.Fatal(err)
	}

	_, err := service.Ingest(context.Background(), "fixture-token", validRequest(), []byte(`{}`))
	if !errors.Is(err, ErrPersistence) {
		t.Fatalf("Ingest() error = %v, want persistence error", err)
	}
	if !strings.Contains(err.Error(), "forced test failure") {
		t.Fatalf("Ingest() error = %v, want trigger failure", err)
	}
	assertReadingsForDevice(t, db, "device-1", 0)

	var lastSeen sql.NullString
	if err := db.QueryRow(`SELECT last_seen FROM devices WHERE id = 'device-1'`).Scan(&lastSeen); err != nil {
		t.Fatal(err)
	}
	if !lastSeen.Valid || lastSeen.String != "before-ingest" {
		t.Fatalf("last_seen = %#v, want before-ingest", lastSeen)
	}
}

func validRequest() Request {
	fill, zero := 72.0, 0.0
	return Request{
		DeviceID:   "SIGAP-01",
		RecordedAt: "2026-09-16T18:00:00+07:00",
		Slots: []Slot{
			{Stream: "organic", Fill: &fill},
			{Stream: "inorganic", Weight: &zero},
			{Stream: "b3"},
		},
	}
}

func testService(t *testing.T) (*sql.DB, *Service) {
	t.Helper()
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	schema := `
		CREATE TABLE devices (id TEXT PRIMARY KEY, device_code TEXT NOT NULL UNIQUE, credential_digest TEXT NOT NULL, last_seen TEXT);
		CREATE TABLE slots (id TEXT PRIMARY KEY, device_id TEXT NOT NULL, waste_stream TEXT NOT NULL, operational_capacity REAL, UNIQUE(device_id, waste_stream));
		CREATE TABLE readings (id INTEGER PRIMARY KEY, slot_id TEXT NOT NULL, recorded_at TEXT NOT NULL, raw_payload TEXT NOT NULL, fill REAL, weight REAL, weight_utilization REAL, ogi REAL, accumulation REAL, temperature REAL, sensor_health TEXT);
		CREATE TABLE decisions (reading_id INTEGER PRIMARY KEY, fis_score REAL, fis_status TEXT, fis_version TEXT, dominant_reason TEXT, safety_state TEXT, trace_json TEXT, safety_json TEXT);
	`
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		t.Fatal(err)
	}
	for _, device := range []struct {
		id, code, token, lastSeen string
	}{
		{"device-1", "SIGAP-01", "fixture-token", "before-ingest"},
		{"device-2", "SIGAP-02", "fixture-token-b", "before-ingest"},
	} {
		digest := sha256.Sum256([]byte(device.token))
		if _, err := db.Exec(`INSERT INTO devices (id, device_code, credential_digest, last_seen) VALUES (?, ?, ?, ?)`, device.id, device.code, hex.EncodeToString(digest[:]), device.lastSeen); err != nil {
			db.Close()
			t.Fatal(err)
		}
	}
	for _, deviceID := range []string{"device-1", "device-2"} {
		for _, stream := range []string{"organic", "inorganic", "b3"} {
			if _, err := db.Exec(`INSERT INTO slots (id, device_id, waste_stream) VALUES (?, ?, ?)`, "slot-"+deviceID+"-"+stream, deviceID, stream); err != nil {
				db.Close()
				t.Fatal(err)
			}
		}
	}
	if _, err := db.Exec(`UPDATE slots SET id = 'slot-b3' WHERE id = 'slot-device-1-b3'`); err != nil {
		db.Close()
		t.Fatal(err)
	}
	return db, NewService(db)
}

func assertReadingsForDevice(t *testing.T, db *sql.DB, deviceID string, want int) {
	t.Helper()
	var count int
	if err := db.QueryRow(`
		SELECT COUNT(*)
		FROM readings
		JOIN slots ON slots.id = readings.slot_id
		WHERE slots.device_id = ?`, deviceID).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != want {
		t.Fatalf("readings for %s = %d, want %d", deviceID, count, want)
	}
}
