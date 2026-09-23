package ingest

import (
	"context"
	"database/sql"
	"encoding/json"
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
	engines      map[string]fuzzy.Engine
	safetyConfig safety.Config
}

// ServiceOption configures source-independent composition seams.
type ServiceOption func(*Service)

// WithDecisionEngine supplies an explicitly configured FIS engine. NewService
// deliberately has no fallback production configuration.
func WithDecisionEngine(engine fuzzy.Engine) ServiceOption {
	return func(service *Service) { service.engine = engine }
}

// WithDecisionProfiles supplies explicitly configured per-stream engines.
// The map is copied so callers cannot change the active profiles after setup.
func WithDecisionProfiles(engines map[string]fuzzy.Engine) ServiceOption {
	return func(service *Service) {
		service.engines = make(map[string]fuzzy.Engine, len(engines))
		for stream, engine := range engines {
			if engine != nil {
				service.engines[stream] = engine
			}
		}
	}
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
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		slotID, recordedAt, rawPayload, input.Fill, input.Weight, normalized.WeightUtilization, normalized.OGI, normalized.AccumulationHours, input.Temperature, health)
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
	engine := s.engineFor(input.Stream)
	var safetyState any
	var safetyJSON any
	safetyConfigured := false
	if input.Stream == "b3" {
		safetyInput, channels := b3SafetyInput(input)
		safetyResult, err := safety.Evaluate(safetyInput, s.safetyConfig)
		if err != nil {
			return fmt.Errorf("%w: evaluate safety: %v", ErrPersistence, err)
		}
		safetyState = safetyResult.State
		safetyConfigured = safetyResult.Configured
		evidence := SafetyEvidence{State: safetyResult.State, Reason: safetyResult.Reason, Channels: channels}
		encoded, err := json.Marshal(evidence)
		if err != nil {
			return fmt.Errorf("%w: encode safety evidence: %v", ErrPersistence, err)
		}
		safetyJSON = string(encoded)
	}
	validation.Reasons = append(validation.Reasons, collectionSensorReasons(input.Sensors)...)
	validation.Valid = len(validation.Reasons) == 0
	if !validation.Valid {
		var version any
		if engine != nil {
			version = engine.Version()
		}
		trace, err := encodeTrace(DecisionTrace{Inputs: fuzzyInputs(normalized), Status: "Data Invalid", Version: stringValue(version), DominantReason: strings.Join(validation.Reasons, "; ")})
		if err != nil {
			return err
		}
		return insertDecision(ctx, tx, readingID, nil, "Data Invalid", version, strings.Join(validation.Reasons, "; "), safetyState, trace, safetyJSON)
	}
	if engine == nil {
		// The runtime has no evidence-backed production FIS configuration yet.
		// Safety evidence remains independently auditable when configured.
		if safetyConfigured {
			return insertDecision(ctx, tx, readingID, nil, nil, nil, "", safetyState, nil, safetyJSON)
		}
		return nil
	}
	inputValues := fuzzyInputs(normalized)
	result, err := engine.Evaluate(inputValues)
	if err != nil {
		if errors.Is(err, fuzzy.ErrMissingInput) || errors.Is(err, fuzzy.ErrNoRuleFired) {
			reason := "fuzzy evaluation unavailable: " + err.Error()
			trace, traceErr := encodeTrace(DecisionTrace{Inputs: inputValues, Status: "Data Invalid", Version: engine.Version(), DominantReason: reason})
			if traceErr != nil {
				return traceErr
			}
			return insertDecision(ctx, tx, readingID, nil, "Data Invalid", engine.Version(), reason, safetyState, trace, safetyJSON)
		}
		return fmt.Errorf("%w: evaluate decision: %v", ErrPersistence, err)
	}
	version := result.Version
	if version == "" {
		version = engine.Version()
	}
	triggered := 0
	for _, rule := range result.Rules {
		if rule.Strength > 0 {
			triggered++
		}
	}
	trace, err := encodeTrace(DecisionTrace{Inputs: inputValues, Memberships: result.InputMemberships, Rules: result.Rules, TriggeredRules: triggered, Score: &result.Score, Status: result.Status, Version: version, DominantReason: result.DominantReason})
	if err != nil {
		return err
	}
	return insertDecision(ctx, tx, readingID, &result.Score, result.Status, version, result.DominantReason, safetyState, trace, safetyJSON)
}

func insertDecision(ctx context.Context, tx *sql.Tx, readingID int64, score *float64, status any, version any, reason string, safetyState, traceJSON, safetyJSON any) error {
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO decisions (reading_id, fis_score, fis_status, fis_version, dominant_reason, safety_state, trace_json, safety_json)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, readingID, score, status, version, reason, safetyState, traceJSON, safetyJSON); err != nil {
		return fmt.Errorf("%w: insert decision: %v", ErrPersistence, err)
	}
	return nil
}

func (s *Service) engineFor(stream string) fuzzy.Engine {
	if engine := s.engines[stream]; engine != nil {
		return engine
	}
	return s.engine
}

func fuzzyInputs(normalized NormalizedReading) fuzzy.Input {
	values := fuzzy.Input{}
	if normalized.FillPct != nil {
		values["fill"] = *normalized.FillPct
	}
	if normalized.WeightUtilization != nil {
		values["weight_utilization"] = *normalized.WeightUtilization
	}
	if normalized.OGI != nil {
		values["ogi"] = *normalized.OGI
	}
	if normalized.AccumulationHours != nil {
		values["accumulation_hours"] = *normalized.AccumulationHours
	}
	return values
}

func encodeTrace(trace DecisionTrace) (string, error) {
	encoded, err := json.Marshal(trace)
	if err != nil {
		return "", fmt.Errorf("%w: encode decision trace: %v", ErrPersistence, err)
	}
	return string(encoded), nil
}

func stringValue(value any) string {
	if text, ok := value.(string); ok {
		return text
	}
	return ""
}

func collectionSensorReasons(sensors []SensorReading) []string {
	reasons := make([]string, 0)
	for _, sensor := range sensors {
		if !collectionSensor(sensor) || sensor.SensorState == SensorValid {
			continue
		}
		reasons = append(reasons, fmt.Sprintf("%s state is %s", sensor.SensorID, sensor.SensorState))
	}
	return reasons
}

func collectionSensor(sensor SensorReading) bool {
	switch strings.ToLower(sensor.Parameter) {
	case "fill", "weight", "broad_gas", "ch4":
		return true
	default:
		return false
	}
}

func b3SafetyInput(slot Slot) (safety.Input, []SafetyChannelEvidence) {
	input := safety.Input{Fill: slot.Fill, Weight: slot.Weight, Temperature: slot.Temperature, GasRaw: slot.GasRaw}
	channels := make([]SafetyChannelEvidence, 0, 3)
	for _, sensor := range slot.Sensors {
		typeName := strings.ToLower(sensor.SensorType)
		if typeName != "mq7" && typeName != "mq8" && typeName != "ds18b20" {
			continue
		}
		channels = append(channels, SafetyChannelEvidence{SensorID: sensor.SensorID, SensorType: sensor.SensorType, Parameter: sensor.Parameter, Value: sensor.EngineeringValue, Normalized: sensor.NormalizedValue, Unit: sensor.Unit, SensorState: sensor.SensorState, RecordedAt: sensor.RecordedAt})
		if sensor.SensorState != SensorValid {
			continue
		}
		evaluation := sensor.NormalizedValue
		if typeName == "ds18b20" {
			evaluation = sensor.EngineeringValue
		}
		switch typeName {
		case "mq7":
			input.MQ7CO = evaluation
		case "mq8":
			input.MQ8H2 = evaluation
		case "ds18b20":
			input.DS18B20Temperature = evaluation
		}
	}
	return input, channels
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
