package ingest

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"path/filepath"
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
		CREATE TABLE slots (id TEXT PRIMARY KEY, device_id TEXT NOT NULL, waste_stream TEXT NOT NULL, UNIQUE(device_id, waste_stream));
		CREATE TABLE readings (id INTEGER PRIMARY KEY, slot_id TEXT NOT NULL, recorded_at TEXT NOT NULL, raw_payload TEXT NOT NULL, fill_pct REAL, weight_kg REAL, ogi REAL, accumulated_mass REAL, temperature_c REAL, sensor_health TEXT);
	`
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		t.Fatal(err)
	}
	digest := sha256.Sum256([]byte("fixture-token"))
	if _, err := db.Exec(`INSERT INTO devices (id, device_code, credential_digest) VALUES ('device-1', 'SIGAP-01', ?)`, hex.EncodeToString(digest[:])); err != nil {
		db.Close()
		t.Fatal(err)
	}
	for id, stream := range []string{"organic", "inorganic", "b3"} {
		if _, err := db.Exec(`INSERT INTO slots (id, device_id, waste_stream) VALUES (?, 'device-1', ?)`, "slot-"+string(rune('1'+id)), stream); err != nil {
			db.Close()
			t.Fatal(err)
		}
	}
	return db, NewService(db)
}
