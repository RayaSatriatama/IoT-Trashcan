package ingest

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/fuzzy"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/safety"
)

var expectedStreams = map[string]struct{}{
	"organic":   {},
	"inorganic": {},
	"b3":        {},
}

// Service persists authentic device readings through an injected SQL database.
type Service struct {
	db           *sql.DB
	now          func() time.Time
	engine       fuzzy.Engine
	safetyConfig safety.Config
}

// ServiceOption configures source-independent composition seams.
type ServiceOption func(*Service)

// WithDecisionEngine supplies an explicitly configured FIS engine. NewService
// deliberately has no fallback production configuration.
func WithDecisionEngine(engine fuzzy.Engine) ServiceOption {
	return func(service *Service) { service.engine = engine }
}

// WithSafetyConfig supplies explicitly sourced B3 safety thresholds.
func WithSafetyConfig(config safety.Config) ServiceOption {
	return func(service *Service) { service.safetyConfig = config }
}

// NewService builds an ingest service over the application's already-open database.
func NewService(db *sql.DB, options ...ServiceOption) *Service {
	service := &Service{db: db, now: time.Now}
	for _, option := range options {
		if option != nil {
			option(service)
		}
	}
	return service
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

	deviceID, err := authenticateDevice(ctx, tx, req.DeviceID, bearerToken)
	if err != nil {
		return Result{}, err
	}
	recordedText := recordedAt.UTC().Format(time.RFC3339Nano)
	if duplicate, err := isExactReplay(ctx, tx, deviceID, recordedText, string(rawPayload)); err != nil {
		return Result{}, err
	} else if duplicate {
		return Result{DeviceID: req.DeviceID, RecordedAt: recordedAt, Deduplicated: true}, nil
	}

	slots, err := deviceSlots(ctx, tx, deviceID)
	if err != nil {
		return Result{}, err
	}
	for _, input := range req.Slots {
		if _, ok := slots[input.Stream]; !ok {
			return Result{}, fmt.Errorf("%w: required %s slot", ErrNotFound, input.Stream)
		}
	}

	health := req.Health
	if len(health) == 0 {
		health = []byte("null")
	}
	for _, input := range req.Slots {
		slot := slots[input.Stream]
		validation := ValidateOperational(input)
		normalized, err := Normalize(input, slot.capacity)
		if err != nil {
			return Result{}, fmt.Errorf("%w: normalize %s: %w", ErrPersistence, input.Stream, err)
		}
		readingID, err := insertReading(ctx, tx, slot.id, recordedText, string(rawPayload), input, normalized, string(health))
		if err != nil {
			return Result{}, err
		}
		if err := s.persistDecision(ctx, tx, readingID, input, normalized, validation); err != nil {
			return Result{}, err
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

type storedSlot struct {
	id       string
	capacity *float64
}

func authenticateDevice(ctx context.Context, tx *sql.Tx, deviceCode, token string) (string, error) {
	var deviceID, storedHash string
	err := tx.QueryRowContext(ctx, `SELECT id, credential_digest FROM devices WHERE device_code = ?`, deviceCode).Scan(&deviceID, &storedHash)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrUnauthorized
	}
	if err != nil {
		return "", fmt.Errorf("%w: find device: %v", ErrPersistence, err)
	}
	if !credentialMatches(token, storedHash) {
		return "", ErrUnauthorized
	}
	return deviceID, nil
}

func isExactReplay(ctx context.Context, tx *sql.Tx, deviceID, recordedAt, rawPayload string) (bool, error) {
	var total, exact int
	err := tx.QueryRowContext(ctx, `
		SELECT COUNT(*), COALESCE(SUM(CASE WHEN readings.raw_payload = ? THEN 1 ELSE 0 END), 0)
		FROM readings
		JOIN slots ON slots.id = readings.slot_id
		WHERE slots.device_id = ? AND readings.recorded_at = ?`, rawPayload, deviceID, recordedAt).Scan(&total, &exact)
	if err != nil {
		return false, fmt.Errorf("%w: check replay: %v", ErrPersistence, err)
	}
	if total == 0 {
		return false, nil
	}
	if total == len(expectedStreams) && exact == total {
		return true, nil
	}
	return false, ErrConflict
}

func deviceSlots(ctx context.Context, tx *sql.Tx, deviceID string) (map[string]storedSlot, error) {
	rows, err := tx.QueryContext(ctx, `SELECT id, waste_stream, operational_capacity FROM slots WHERE device_id = ?`, deviceID)
	if err != nil {
		return nil, fmt.Errorf("%w: find device slots: %v", ErrPersistence, err)
	}
	defer rows.Close()
	slots := make(map[string]storedSlot, len(expectedStreams))
	for rows.Next() {
		var id, stream string
		var capacity sql.NullFloat64
		if err := rows.Scan(&id, &stream, &capacity); err != nil {
			return nil, fmt.Errorf("%w: scan device slots: %v", ErrPersistence, err)
		}
		var value *float64
		if capacity.Valid {
			value = &capacity.Float64
		}
		slots[stream] = storedSlot{id: id, capacity: value}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: iterate device slots: %v", ErrPersistence, err)
	}
	return slots, nil
}

func insertReading(ctx context.Context, tx *sql.Tx, slotID, recordedAt, rawPayload string, input Slot, normalized NormalizedReading, health string) (int64, error) {
	result, err := tx.ExecContext(ctx, `
		INSERT INTO readings (slot_id, recorded_at, raw_payload, fill, weight, weight_utilization, ogi, accumulation, temperature, sensor_health)
		VALUES (?, ?, ?, ?, ?, ?, NULL, NULL, ?, ?)`,
		slotID, recordedAt, rawPayload, input.Fill, input.Weight, normalized.WeightUtilization, input.Temperature, health)
	if err != nil {
		return 0, fmt.Errorf("%w: insert reading: %v", ErrPersistence, err)
	}
	id, err := result.LastInsertId()
	if err != nil {
		return 0, fmt.Errorf("%w: read inserted reading id: %v", ErrPersistence, err)
	}
	return id, nil
}

func (s *Service) persistDecision(ctx context.Context, tx *sql.Tx, readingID int64, input Slot, normalized NormalizedReading, validation ValidationResult) error {
	safetyResult := safety.Evaluate(safety.Input{Fill: input.Fill, Weight: input.Weight, Temperature: input.Temperature, GasRaw: input.GasRaw}, s.safetyConfig)
	if !validation.Valid {
		var version any
		if s.engine != nil {
			version = s.engine.Version()
		}
		return insertDecision(ctx, tx, readingID, nil, "Data Invalid", version, strings.Join(validation.Reasons, "; "), safetyResult.State)
	}
	if s.engine == nil {
		// The runtime has no evidence-backed production FIS configuration yet.
		// Raw readings and heartbeat remain available, but no decision is forged.
		return nil
	}
	inputValues := fuzzy.Input{}
	if normalized.FillPct != nil {
		inputValues["fill"] = *normalized.FillPct
	}
	if normalized.WeightUtilization != nil {
		inputValues["weight_utilization"] = *normalized.WeightUtilization
	}
	if normalized.OGI != nil {
		inputValues["ogi"] = *normalized.OGI
	}
	if normalized.AccumulationHours != nil {
		inputValues["accumulation_hours"] = *normalized.AccumulationHours
	}
	result, err := s.engine.Evaluate(inputValues)
	if err != nil {
		return fmt.Errorf("%w: evaluate decision: %v", ErrPersistence, err)
	}
	return insertDecision(ctx, tx, readingID, &result.Score, result.Status, result.Version, result.DominantReason, safetyResult.State)
}

func insertDecision(ctx context.Context, tx *sql.Tx, readingID int64, score *float64, status string, version any, reason, safetyState string) error {
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO decisions (reading_id, fis_score, fis_status, fis_version, dominant_reason, safety_state)
		VALUES (?, ?, ?, ?, ?, ?)`, readingID, score, status, version, reason, safetyState); err != nil {
		return fmt.Errorf("%w: insert decision: %v", ErrPersistence, err)
	}
	return nil
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
