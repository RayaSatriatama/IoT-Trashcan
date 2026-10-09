package telemetryarchive

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
)

const archiveToken = "archive-test-token"

func openArchiveDB(t *testing.T) *sql.DB {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "archive.db"))
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256([]byte(archiveToken))
	_, err = db.Exec(`
		INSERT INTO locations (id, name) VALUES ('loc-archive', 'Archive')`)
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.Exec(`
		INSERT INTO devices (id, location_id, device_code, credential_digest)
		VALUES ('device-archive', 'loc-archive', 'SIGAP-01', ?)`, hex.EncodeToString(digest[:]))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

func archiveBody(t *testing.T) []byte {
	t.Helper()
	body, err := os.ReadFile(filepath.Join("..", "..", "contracts", "telemetry", "v3", "body", "body-minimal.json"))
	if err != nil {
		t.Fatal(err)
	}
	return body
}

func TestIngestPersistsExactBodyAndRecords(t *testing.T) {
	db := openArchiveDB(t)
	service := NewService(db)
	body := archiveBody(t)
	result, err := service.Ingest(context.Background(), archiveToken, body)
	if err != nil {
		t.Fatal(err)
	}
	if result.Ack.StoredCount != 1 || result.Ack.DeduplicatedCount != 0 || result.Ack.Status != "DURABLE" {
		t.Fatalf("unexpected ack: %+v", result.Ack)
	}
	var persisted []byte
	if err := db.QueryRow(`SELECT body_bytes FROM telemetry_batches WHERE device_id = 'SIGAP-01'`).Scan(&persisted); err != nil {
		t.Fatal(err)
	}
	if string(persisted) != string(body) {
		t.Fatal("archive did not preserve exact request bytes")
	}
	var recordBytes []byte
	if err := db.QueryRow(`SELECT record_bytes FROM telemetry_records WHERE device_id = 'SIGAP-01'`).Scan(&recordBytes); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(recordBytes), `"record_id":"SIGAP-01/boot-A/1"`) {
		t.Fatalf("record bytes missing identity: %s", recordBytes)
	}
	var bootID, sequenceSort, stream, sensorID, quality string
	if err := db.QueryRow(`SELECT boot_id, sequence_sort, stream, sensor_id, quality FROM telemetry_records WHERE device_id = 'SIGAP-01'`).Scan(&bootID, &sequenceSort, &stream, &sensorID, &quality); err != nil {
		t.Fatal(err)
	}
	if bootID != "boot-A" || sequenceSort != "00000000000000000001" || stream != "organic" || sensorID != "mq4" || quality != "UNCALIBRATED" {
		t.Fatalf("record index mismatch boot=%s seq=%s stream=%s sensor=%s quality=%s", bootID, sequenceSort, stream, sensorID, quality)
	}
}

func TestExactReplayDeduplicatesAndConflictDoesNotMutate(t *testing.T) {
	db := openArchiveDB(t)
	service := NewService(db)
	body := archiveBody(t)
	first, err := service.Ingest(context.Background(), archiveToken, body)
	if err != nil {
		t.Fatal(err)
	}
	second, err := service.Ingest(context.Background(), archiveToken, body)
	if err != nil {
		t.Fatal(err)
	}
	if second.Ack.StoredCount != 0 || second.Ack.DeduplicatedCount != 1 {
		t.Fatalf("unexpected replay ack: %+v", second.Ack)
	}
	// A byte-level change with the same validated batch identity is a replay
	// conflict, even though its semantic JSON is unchanged.
	conflict := append(append([]byte(nil), body...), '\n')
	if _, err := service.Ingest(context.Background(), archiveToken, conflict); !errors.Is(err, ErrConflict) {
		t.Fatalf("conflict error = %v", err)
	}
	var batches, records int
	if err := db.QueryRow(`SELECT COUNT(*) FROM telemetry_batches`).Scan(&batches); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM telemetry_records`).Scan(&records); err != nil {
		t.Fatal(err)
	}
	if batches != 1 || records != 1 || first.Ack.BodyHash != second.Ack.BodyHash {
		t.Fatalf("conflict/replay changed archive batches=%d records=%d", batches, records)
	}
}

func TestAuthenticationAndValidationPrecedeMutation(t *testing.T) {
	db := openArchiveDB(t)
	service := NewService(db)
	body := archiveBody(t)
	if _, err := service.Ingest(context.Background(), "wrong", body); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("wrong token error = %v", err)
	}
	if _, err := service.Ingest(context.Background(), archiveToken, []byte(`{"schema_version":3}`)); err == nil {
		t.Fatal("invalid batch unexpectedly accepted")
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM telemetry_batches`).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 0 {
		t.Fatalf("invalid requests mutated archive: %d", count)
	}
}
