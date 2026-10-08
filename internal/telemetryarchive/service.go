// Package telemetryarchive implements the durable telemetry-v3 ingest boundary.
// It deliberately stores the request bytes as received; archive replay must not
// depend on the serializer used by a later firmware or server release.
package telemetryarchive

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
)

var (
	ErrUnauthorized = errors.New("telemetry archive: unauthorized")
	ErrConflict     = errors.New("telemetry archive: conflict")
	ErrPersistence  = errors.New("telemetry archive: persistence failure")
)

// Result is the durable acknowledgement produced after the transaction has
// committed. It is safe for a handler to encode Ack only after Ingest returns.
type Result struct {
	Ack telemetry.Ack
}

// Service persists validated v3 batches in an injected SQLite database.
type Service struct {
	db  *sql.DB
	now func() time.Time
}

// NewService composes the archive service over an already migrated database.
func NewService(db *sql.DB) *Service { return &Service{db: db, now: time.Now} }

// Ingest authenticates the batch owner, validates the complete contract, and
// atomically stores its exact body plus each typed record. Replays of an exact
// batch are durable deduplications; a reused batch ID with different bytes is a
// conflict and never mutates the archive.
func (s *Service) Ingest(ctx context.Context, bearerToken string, body []byte) (Result, error) {
	if s == nil || s.db == nil {
		return Result{}, fmt.Errorf("%w: database is unavailable", ErrPersistence)
	}
	batch, err := telemetry.ValidateBatch(body)
	if err != nil {
		return Result{}, err
	}
	bodyHash := telemetry.BodyHash(body)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Result{}, fmt.Errorf("%w: begin: %v", ErrPersistence, err)
	}
	defer tx.Rollback()
	if err := authenticate(ctx, tx, batch.DeviceID, bearerToken); err != nil {
		return Result{}, err
	}

	var storedBodyHash string
	err = tx.QueryRowContext(ctx, `
		SELECT body_hash FROM telemetry_batches
		WHERE device_id = ? AND batch_id = ?`, batch.DeviceID, batch.BatchID).Scan(&storedBodyHash)
	if err == nil {
		if storedBodyHash != bodyHash {
			return Result{}, ErrConflict
		}
		if err := tx.Commit(); err != nil {
			return Result{}, fmt.Errorf("%w: replay commit: %v", ErrPersistence, err)
		}
		return Result{Ack: telemetry.Ack{
			DeviceID: batch.DeviceID, BatchID: batch.BatchID, BodyHash: bodyHash,
			RecordCount: uint32(batch.RecordCount), DeduplicatedCount: uint32(batch.RecordCount), Status: "DURABLE",
		}}, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return Result{}, fmt.Errorf("%w: lookup batch: %v", ErrPersistence, err)
	}

	records, err := rawRecords(body)
	if err != nil {
		return Result{}, fmt.Errorf("%w: decode records: %v", ErrPersistence, err)
	}
	created := s.now().UTC().Format(time.RFC3339Nano)
	// Publish the manifest inside the same transaction before its child
	// bindings, then fill the counts only after every record has succeeded.
	// Readers never observe this provisional row because the transaction is
	// committed atomically.
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO telemetry_batches
		(device_id, batch_id, body_hash, body_bytes, record_count, stored_count, deduplicated_count, status, created_at)
		VALUES (?, ?, ?, ?, ?, 0, 0, 'DURABLE', ?)`,
		batch.DeviceID, batch.BatchID, bodyHash, body, batch.RecordCount, created); err != nil {
		return Result{}, fmt.Errorf("%w: insert batch: %v", ErrPersistence, err)
	}
	stored, deduplicated, err := s.insertRecords(ctx, tx, batch, records, created)
	if err != nil {
		return Result{}, err
	}
	if _, err := tx.ExecContext(ctx, `
		UPDATE telemetry_batches SET stored_count = ?, deduplicated_count = ?
		WHERE device_id = ? AND batch_id = ?`, stored, deduplicated, batch.DeviceID, batch.BatchID); err != nil {
		return Result{}, fmt.Errorf("%w: update batch counts: %v", ErrPersistence, err)
	}
	if err := tx.Commit(); err != nil {
		return Result{}, fmt.Errorf("%w: commit: %v", ErrPersistence, err)
	}
	return Result{Ack: telemetry.Ack{
		DeviceID: batch.DeviceID, BatchID: batch.BatchID, BodyHash: bodyHash,
		RecordCount: uint32(batch.RecordCount), StoredCount: uint32(stored),
		DeduplicatedCount: uint32(deduplicated), Status: "DURABLE",
	}}, nil
}

type rawRecord struct {
	id, hash, kind string
	bytes          []byte
}

func rawRecords(body []byte) ([]rawRecord, error) {
	var envelope struct {
		Samples  []json.RawMessage `json:"samples"`
		Results  []json.RawMessage `json:"results"`
		Events   []json.RawMessage `json:"events"`
		Metadata []json.RawMessage `json:"metadata"`
	}
	if err := json.Unmarshal(body, &envelope); err != nil {
		return nil, err
	}
	result := make([]rawRecord, 0, len(envelope.Samples)+len(envelope.Results)+len(envelope.Events)+len(envelope.Metadata))
	appendRecords := func(values []json.RawMessage, kind string) error {
		for _, raw := range values {
			var fields struct {
				RecordID   string `json:"record_id"`
				RecordHash string `json:"record_hash"`
			}
			if err := json.Unmarshal(raw, &fields); err != nil {
				return err
			}
			result = append(result, rawRecord{id: fields.RecordID, hash: fields.RecordHash, kind: kind, bytes: append([]byte(nil), raw...)})
		}
		return nil
	}
	for _, group := range []struct {
		values []json.RawMessage
		kind   string
	}{{envelope.Samples, "sample"}, {envelope.Results, "result"}, {envelope.Events, "event"}, {envelope.Metadata, "metadata"}} {
		if err := appendRecords(group.values, group.kind); err != nil {
			return nil, err
		}
	}
	return result, nil
}

func (s *Service) insertRecords(ctx context.Context, tx *sql.Tx, batch *telemetry.Batch, records []rawRecord, created string) (int, int, error) {
	stored, deduplicated := 0, 0
	for _, record := range records {
		var existingHash string
		err := tx.QueryRowContext(ctx, `SELECT record_hash FROM telemetry_records WHERE device_id = ? AND record_id = ?`, batch.DeviceID, record.id).Scan(&existingHash)
		switch {
		case err == nil:
			if existingHash != record.hash {
				return 0, 0, ErrConflict
			}
			deduplicated++
		case errors.Is(err, sql.ErrNoRows):
			if _, err := tx.ExecContext(ctx, `
				INSERT INTO telemetry_records (device_id, record_id, record_hash, kind, record_bytes, first_batch_id, created_at)
				VALUES (?, ?, ?, ?, ?, ?, ?)`, batch.DeviceID, record.id, record.hash, record.kind, record.bytes, batch.BatchID, created); err != nil {
				return 0, 0, fmt.Errorf("%w: insert record: %v", ErrPersistence, err)
			}
			stored++
		default:
			return 0, 0, fmt.Errorf("%w: lookup record: %v", ErrPersistence, err)
		}
		if _, err := tx.ExecContext(ctx, `
			INSERT INTO telemetry_batch_records (device_id, batch_id, record_id, record_hash)
			VALUES (?, ?, ?, ?)`, batch.DeviceID, batch.BatchID, record.id, record.hash); err != nil {
			return 0, 0, fmt.Errorf("%w: bind record: %v", ErrPersistence, err)
		}
	}
	return stored, deduplicated, nil
}

func authenticate(ctx context.Context, tx *sql.Tx, deviceID, token string) error {
	var digest string
	err := tx.QueryRowContext(ctx, `SELECT credential_digest FROM devices WHERE device_code = ?`, deviceID).Scan(&digest)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnauthorized
	}
	if err != nil {
		return fmt.Errorf("%w: device lookup: %v", ErrPersistence, err)
	}
	if token == "" {
		return ErrUnauthorized
	}
	want, err := hex.DecodeString(digest)
	if err != nil || len(want) != sha256.Size {
		return ErrUnauthorized
	}
	got := sha256.Sum256([]byte(token))
	if subtle.ConstantTimeCompare(got[:], want) != 1 {
		return ErrUnauthorized
	}
	return nil
}
