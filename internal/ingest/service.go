package ingest

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

var expectedStreams = map[string]struct{}{
	"organic":   {},
	"inorganic": {},
	"b3":        {},
}

// Service persists authentic device readings through an injected SQL database.
type Service struct {
	db  *sql.DB
	now func() time.Time
}

// NewService builds an ingest service over the application's already-open database.
func NewService(db *sql.DB) *Service {
	return &Service{db: db, now: time.Now}
}

// Ingest validates a device payload and atomically persists its three raw readings.
func (s *Service) Ingest(ctx context.Context, bearerToken string, req Request, rawPayload []byte) (Result, error) {
	recordedAt, err := validate(req)
	if err != nil {
		return Result{}, err
	}
	if s == nil || s.db == nil {
		return Result{}, fmt.Errorf("%w: database is unavailable", ErrPersistence)
	}

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Result{}, fmt.Errorf("%w: begin transaction: %v", ErrPersistence, err)
	}
	defer tx.Rollback()

	var deviceID string
	var storedHash string
	err = tx.QueryRowContext(ctx, `SELECT id, credential_digest FROM devices WHERE device_code = ?`, req.DeviceID).Scan(&deviceID, &storedHash)
	if errors.Is(err, sql.ErrNoRows) {
		return Result{}, ErrUnauthorized
	}
	if err != nil {
		return Result{}, fmt.Errorf("%w: find device: %v", ErrPersistence, err)
	}
	if !credentialMatches(bearerToken, storedHash) {
		return Result{}, ErrUnauthorized
	}

	slotIDs := make(map[string]string, len(req.Slots))
	rows, err := tx.QueryContext(ctx, `SELECT id, waste_stream FROM slots WHERE device_id = ?`, deviceID)
	if err != nil {
		return Result{}, fmt.Errorf("%w: find device slots: %v", ErrPersistence, err)
	}
	for rows.Next() {
		var id string
		var stream string
		if err := rows.Scan(&id, &stream); err != nil {
			rows.Close()
			return Result{}, fmt.Errorf("%w: scan device slots: %v", ErrPersistence, err)
		}
		slotIDs[stream] = id
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return Result{}, fmt.Errorf("%w: iterate device slots: %v", ErrPersistence, err)
	}
	if err := rows.Close(); err != nil {
		return Result{}, fmt.Errorf("%w: close device slots: %v", ErrPersistence, err)
	}
	for _, input := range req.Slots {
		if _, ok := slotIDs[input.Stream]; !ok {
			return Result{}, fmt.Errorf("%w: required %s slot", ErrNotFound, input.Stream)
		}
	}

	health := req.Health
	if len(health) == 0 {
		health = []byte("null")
	}
	recordedText := recordedAt.UTC().Format(time.RFC3339Nano)
	for _, input := range req.Slots {
		_, err = tx.ExecContext(ctx, `
			INSERT INTO readings (slot_id, recorded_at, raw_payload, fill_pct, weight_kg, ogi, accumulated_mass, temperature_c, sensor_health)
			VALUES (?, ?, ?, ?, ?, NULL, NULL, ?, ?)`,
			slotIDs[input.Stream], recordedText, string(rawPayload), input.Fill, input.Weight, input.Temperature, string(health))
		if err != nil {
			return Result{}, fmt.Errorf("%w: insert %s reading: %v", ErrPersistence, input.Stream, err)
		}
	}

	if _, err = tx.ExecContext(ctx, `UPDATE devices SET last_seen = ? WHERE id = ?`, s.now().UTC().Format(time.RFC3339Nano), deviceID); err != nil {
		return Result{}, fmt.Errorf("%w: update heartbeat: %v", ErrPersistence, err)
	}
	if err = tx.Commit(); err != nil {
		return Result{}, fmt.Errorf("%w: commit transaction: %v", ErrPersistence, err)
	}

	return Result{DeviceID: req.DeviceID, RecordedAt: recordedAt, ReadingsStored: len(req.Slots)}, nil
}

func validate(req Request) (time.Time, error) {
	if strings.TrimSpace(req.DeviceID) == "" {
		return time.Time{}, fmt.Errorf("%w: device_id is required", ErrInvalidRequest)
	}
	recordedAt, err := time.Parse(time.RFC3339, req.RecordedAt)
	if err != nil {
		return time.Time{}, fmt.Errorf("%w: recorded_at must be RFC3339", ErrInvalidRequest)
	}
	if len(req.Slots) != len(expectedStreams) {
		return time.Time{}, fmt.Errorf("%w: exactly three slots are required", ErrInvalidRequest)
	}
	seen := make(map[string]struct{}, len(req.Slots))
	for _, slot := range req.Slots {
		if _, wanted := expectedStreams[slot.Stream]; !wanted {
			return time.Time{}, fmt.Errorf("%w: unsupported stream %q", ErrInvalidRequest, slot.Stream)
		}
		if _, duplicate := seen[slot.Stream]; duplicate {
			return time.Time{}, fmt.Errorf("%w: duplicate stream %q", ErrInvalidRequest, slot.Stream)
		}
		seen[slot.Stream] = struct{}{}
	}
	return recordedAt, nil
}
