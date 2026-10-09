// Package devicecontrol provides a durable, authenticated command boundary for
// remote calibration.  It owns no credentials and never manufactures sensor
// evidence; the device remains the calibration authority.
package devicecontrol

import (
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

const MaxPayloadBytes = 16 * 1024

type Status string

const (
	Queued       Status = "QUEUED"
	Delivered    Status = "DELIVERED"
	Accepted     Status = "ACCEPTED"
	Capturing    Status = "CAPTURING"
	Validated    Status = "VALIDATED"
	PreviewReady Status = "PREVIEW_READY"
	Committed    Status = "COMMITTED"
	Failed       Status = "FAILED"
	Expired      Status = "EXPIRED"
	Cancelled    Status = "CANCELLED"
)

var (
	ErrConflict     = errors.New("device control: command conflict")
	ErrNotFound     = errors.New("device control: command not found")
	ErrSessionBusy  = errors.New("device control: calibration session busy")
	ErrExpired      = errors.New("device control: command expired")
	ErrInvalid      = errors.New("device control: invalid command")
	ErrUnauthorized = errors.New("device control: unauthorized")
)

type Command struct {
	ID            string          `json:"command_id"`
	DeviceID      string          `json:"device_id"`
	OperatorID    string          `json:"operator_id"`
	SessionID     string          `json:"session_id,omitempty"`
	SensorID      string          `json:"sensor_id"`
	Operation     string          `json:"operation"`
	Payload       json.RawMessage `json:"payload"`
	RequestHash   string          `json:"request_hash"`
	Status        Status          `json:"status"`
	ExpiresAt     time.Time       `json:"expires_at"`
	Attempt       int             `json:"attempt"`
	ResultPayload json.RawMessage `json:"result,omitempty"`
	ResultHash    string          `json:"result_hash,omitempty"`
	ErrorCode     string          `json:"error_code,omitempty"`
}

type CreateRequest struct {
	ID         string          `json:"command_id"`
	DeviceID   string          `json:"device_id"`
	OperatorID string          `json:"operator_id"`
	SessionID  string          `json:"session_id,omitempty"`
	SensorID   string          `json:"sensor_id"`
	Operation  string          `json:"operation"`
	Payload    json.RawMessage `json:"payload"`
	ExpiresAt  time.Time       `json:"expires_at"`
}

type ResultRequest struct {
	Status    Status          `json:"status"`
	SessionID string          `json:"session_id"`
	Payload   json.RawMessage `json:"result,omitempty"`
	ErrorCode string          `json:"error_code,omitempty"`
}

type Store struct {
	db    *sql.DB
	now   func() time.Time
	lease time.Duration
}

func NewStore(db *sql.DB) *Store { return &Store{db: db, now: time.Now, lease: 30 * time.Second} }
func (s *Store) WithClock(now func() time.Time) *Store {
	if now != nil {
		s.now = now
	}
	return s
}
func (s *Store) WithLease(d time.Duration) *Store {
	if d > 0 {
		s.lease = d
	}
	return s
}

func hashPayload(payload []byte) string {
	sum := sha256.Sum256(payload)
	return "sha256:" + hex.EncodeToString(sum[:])
}
func validStatus(v Status) bool {
	switch v {
	case Accepted, Capturing, Validated, PreviewReady, Committed, Failed, Cancelled:
		return true
	}
	return false
}
func terminal(v Status) bool { return v == Committed || v == Failed || v == Expired || v == Cancelled }

func validTransition(from, to Status) bool {
	if to == Failed || to == Cancelled {
		return from == Delivered || from == Accepted || from == Capturing || from == Validated || from == PreviewReady
	}
	switch from {
	case Delivered:
		return to == Accepted || to == Capturing
	case Accepted:
		return to == Capturing
	case Capturing:
		return to == Validated
	case Validated:
		return to == PreviewReady
	case PreviewReady:
		return to == Committed
	default:
		return false
	}
}

func validCommittedResult(payload []byte) bool {
	var value struct {
		ProfileKey  string `json:"profile_key"`
		ProfileHash string `json:"profile_hash"`
		Version     uint64 `json:"version"`
	}
	if len(payload) == 0 || json.Unmarshal(payload, &value) != nil {
		return false
	}
	return strings.TrimSpace(value.ProfileKey) != "" &&
		strings.TrimSpace(value.ProfileHash) != "" && value.Version > 0
}

func (s *Store) Enqueue(ctx context.Context, req CreateRequest) (Command, error) {
	if s == nil || s.db == nil || strings.TrimSpace(req.ID) == "" || strings.TrimSpace(req.DeviceID) == "" || strings.TrimSpace(req.OperatorID) == "" || strings.TrimSpace(req.SensorID) == "" || strings.TrimSpace(req.Operation) == "" || len(req.Payload) == 0 || len(req.Payload) > MaxPayloadBytes || req.ExpiresAt.IsZero() {
		return Command{}, ErrInvalid
	}
	if req.ExpiresAt.Before(s.now().UTC()) {
		return Command{}, ErrExpired
	}
	var object any
	dec := json.Unmarshal(req.Payload, &object)
	if dec != nil {
		return Command{}, ErrInvalid
	}
	hash := hashPayload(req.Payload)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Command{}, err
	}
	defer tx.Rollback()
	var existing Command
	var payload []byte
	var resultPayload []byte
	var exp string
	var status string
	err = tx.QueryRowContext(ctx, `SELECT command_id,device_id,operator_id,COALESCE(session_id,''),sensor_id,operation,payload,request_hash,status,expires_at,attempt,COALESCE(result_payload,''),COALESCE(result_hash,''),COALESCE(error_code,'') FROM device_control_commands WHERE command_id=?`, req.ID).Scan(&existing.ID, &existing.DeviceID, &existing.OperatorID, &existing.SessionID, &existing.SensorID, &existing.Operation, &payload, &existing.RequestHash, &status, &exp, &existing.Attempt, &resultPayload, &existing.ResultHash, &existing.ErrorCode)
	if err == nil {
		if existing.RequestHash != hash || existing.DeviceID != req.DeviceID {
			return Command{}, ErrConflict
		}
		existing.Payload = payload
		existing.ResultPayload = resultPayload
		existing.Status = Status(status)
		existing.ExpiresAt, _ = time.Parse(time.RFC3339Nano, exp)
		return existing, tx.Commit()
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return Command{}, err
	}
	if strings.EqualFold(req.Operation, "start") {
		var sid, owner, state, oldExp string
		err = tx.QueryRowContext(ctx, `SELECT session_id,operator_id,state,expires_at FROM device_control_sessions WHERE device_id=?`, req.DeviceID).Scan(&sid, &owner, &state, &oldExp)
		if err == nil {
			old, _ := time.Parse(time.RFC3339Nano, oldExp)
			if state == "ACTIVE" && old.After(s.now().UTC()) && (owner != req.OperatorID || sid != req.SessionID) {
				return Command{}, ErrSessionBusy
			}
		}
		if errors.Is(err, sql.ErrNoRows) {
			_, err = tx.ExecContext(ctx, `INSERT INTO device_control_sessions(device_id,session_id,operator_id,sensor_id,expires_at,state,created_at,updated_at) VALUES(?,?,?,?,?,'ACTIVE',?,?)`, req.DeviceID, req.SessionID, req.OperatorID, req.SensorID, req.ExpiresAt.UTC().Format(time.RFC3339Nano), s.now().UTC().Format(time.RFC3339Nano), s.now().UTC().Format(time.RFC3339Nano))
		} else if err == nil {
			_, err = tx.ExecContext(ctx, `UPDATE device_control_sessions SET session_id=?,operator_id=?,sensor_id=?,expires_at=?,state='ACTIVE',updated_at=? WHERE device_id=?`, req.SessionID, req.OperatorID, req.SensorID, req.ExpiresAt.UTC().Format(time.RFC3339Nano), s.now().UTC().Format(time.RFC3339Nano), req.DeviceID)
		}
		if err != nil {
			return Command{}, err
		}
	}
	now := s.now().UTC().Format(time.RFC3339Nano)
	_, err = tx.ExecContext(ctx, `INSERT INTO device_control_commands(command_id,device_id,operator_id,session_id,sensor_id,operation,payload,request_hash,status,expires_at,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,'QUEUED',?,?,?)`, req.ID, req.DeviceID, req.OperatorID, req.SessionID, req.SensorID, req.Operation, req.Payload, hash, req.ExpiresAt.UTC().Format(time.RFC3339Nano), now, now)
	if err != nil {
		return Command{}, err
	}
	if err = tx.Commit(); err != nil {
		return Command{}, err
	}
	return Command{ID: req.ID, DeviceID: req.DeviceID, OperatorID: req.OperatorID, SessionID: req.SessionID, SensorID: req.SensorID, Operation: req.Operation, Payload: append([]byte(nil), req.Payload...), RequestHash: hash, Status: Queued, ExpiresAt: req.ExpiresAt, Attempt: 0}, nil
}

func (s *Store) Claim(ctx context.Context, deviceID string) (Command, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Command{}, err
	}
	defer tx.Rollback()
	now := s.now().UTC()
	nowText := now.Format(time.RFC3339Nano)
	_, _ = tx.ExecContext(ctx, `UPDATE device_control_commands SET status='EXPIRED',lease_until=NULL,updated_at=? WHERE device_id=? AND status IN ('QUEUED','DELIVERED') AND expires_at <= ?`, nowText, deviceID, nowText)
	// A delivered command whose lease elapsed is eligible for redelivery. It
	// remains the same command identity and payload, and therefore cannot
	// create a second calibration side effect by itself.
	_, _ = tx.ExecContext(ctx, `UPDATE device_control_commands SET status='QUEUED',lease_until=NULL,updated_at=? WHERE device_id=? AND status='DELIVERED' AND lease_until IS NOT NULL AND lease_until <= ? AND expires_at > ?`, nowText, deviceID, nowText, nowText)
	_, _ = tx.ExecContext(ctx, `UPDATE device_control_sessions SET state='CLOSED',updated_at=? WHERE device_id=? AND state='ACTIVE' AND expires_at <= ?`, nowText, deviceID, nowText)
	var c Command
	var payload []byte
	var resultPayload []byte
	var exp string
	err = tx.QueryRowContext(ctx, `SELECT command_id,device_id,operator_id,COALESCE(session_id,''),sensor_id,operation,payload,request_hash,status,expires_at,attempt,COALESCE(result_payload,''),COALESCE(result_hash,''),COALESCE(error_code,'') FROM device_control_commands WHERE device_id=? AND status='QUEUED' ORDER BY created_at,command_id LIMIT 1`, deviceID).Scan(&c.ID, &c.DeviceID, &c.OperatorID, &c.SessionID, &c.SensorID, &c.Operation, &payload, &c.RequestHash, &c.Status, &exp, &c.Attempt, &resultPayload, &c.ResultHash, &c.ErrorCode)
	if errors.Is(err, sql.ErrNoRows) {
		return Command{}, ErrNotFound
	}
	if err != nil {
		return Command{}, err
	}
	c.Payload = payload
	c.ResultPayload = resultPayload
	c.ExpiresAt, _ = time.Parse(time.RFC3339Nano, exp)
	c.Attempt++
	lease := now.Add(s.lease).Format(time.RFC3339Nano)
	if _, err = tx.ExecContext(ctx, `UPDATE device_control_commands SET status='DELIVERED',lease_until=?,attempt=?,updated_at=? WHERE command_id=? AND status='QUEUED'`, lease, c.Attempt, nowText, c.ID); err != nil {
		return Command{}, err
	}
	if err = tx.Commit(); err != nil {
		return Command{}, err
	}
	c.Status = Delivered
	return c, nil
}

func (s *Store) UpdateResult(ctx context.Context, deviceID, commandID string, result ResultRequest) (Command, error) {
	if !validStatus(result.Status) || len(result.Payload) > MaxPayloadBytes {
		return Command{}, ErrInvalid
	}
	if result.Status == Committed && !validCommittedResult(result.Payload) {
		return Command{}, ErrInvalid
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Command{}, err
	}
	defer tx.Rollback()
	var c Command
	var payload []byte
	var exp string
	var status string
	err = tx.QueryRowContext(ctx, `SELECT command_id,device_id,operator_id,COALESCE(session_id,''),sensor_id,operation,payload,request_hash,status,expires_at,attempt FROM device_control_commands WHERE command_id=? AND device_id=?`, commandID, deviceID).Scan(&c.ID, &c.DeviceID, &c.OperatorID, &c.SessionID, &c.SensorID, &c.Operation, &payload, &c.RequestHash, &status, &exp, &c.Attempt)
	if errors.Is(err, sql.ErrNoRows) {
		return Command{}, ErrNotFound
	}
	if err != nil {
		return Command{}, err
	}
	now := s.now().UTC()
	expires, _ := time.Parse(time.RFC3339Nano, exp)
	if !expires.After(now) && !terminal(Status(status)) {
		return Command{}, ErrExpired
	}
	if result.SessionID != "" && result.SessionID != c.SessionID {
		return Command{}, ErrConflict
	}
	resultHash := hashPayload(result.Payload)
	if terminal(Status(status)) {
		// Lost HTTP responses are safely replayable only when every result
		// field and bytes match the durable terminal result.
		var storedPayload []byte
		var storedHash string
		if err := tx.QueryRowContext(ctx, `SELECT COALESCE(result_payload,''),COALESCE(result_hash,'') FROM device_control_commands WHERE command_id=?`, commandID).Scan(&storedPayload, &storedHash); err != nil {
			return Command{}, err
		}
		if Status(status) == result.Status && storedHash == resultHash && bytes.Equal(storedPayload, result.Payload) {
			c.Payload = payload
			c.Status = Status(status)
			c.ResultPayload = storedPayload
			c.ResultHash = storedHash
			c.ExpiresAt = expires
			_ = tx.Rollback()
			return c, nil
		}
		return Command{}, ErrConflict
	}
	if !validTransition(Status(status), result.Status) {
		return Command{}, ErrInvalid
	}
	nowText := now.Format(time.RFC3339Nano)
	_, err = tx.ExecContext(ctx, `UPDATE device_control_commands SET status=?,result_payload=?,result_hash=?,error_code=?,updated_at=? WHERE command_id=? AND status NOT IN ('COMMITTED','FAILED','EXPIRED','CANCELLED')`, result.Status, result.Payload, resultHash, result.ErrorCode, nowText, commandID)
	if err != nil {
		return Command{}, err
	}
	if result.Status == Committed || result.Status == Cancelled || result.Status == Failed {
		_, err = tx.ExecContext(ctx, `UPDATE device_control_sessions SET state='CLOSED',updated_at=? WHERE device_id=? AND session_id=?`, nowText, deviceID, c.SessionID)
		if err != nil {
			return Command{}, err
		}
	}
	if err = tx.Commit(); err != nil {
		return Command{}, err
	}
	c.Payload = payload
	c.Status = result.Status
	c.ResultPayload = append([]byte(nil), result.Payload...)
	c.ResultHash = resultHash
	c.ErrorCode = result.ErrorCode
	c.ExpiresAt = expires
	return c, nil
}

func (s *Store) Get(ctx context.Context, deviceID, commandID string) (Command, error) {
	var c Command
	var payload, result []byte
	var exp, status string
	err := s.db.QueryRowContext(ctx, `SELECT command_id,device_id,operator_id,COALESCE(session_id,''),sensor_id,operation,payload,request_hash,status,expires_at,attempt,COALESCE(result_payload,''),COALESCE(result_hash,''),COALESCE(error_code,'') FROM device_control_commands WHERE device_id=? AND command_id=?`, deviceID, commandID).Scan(&c.ID, &c.DeviceID, &c.OperatorID, &c.SessionID, &c.SensorID, &c.Operation, &payload, &c.RequestHash, &status, &exp, &c.Attempt, &result, &c.ResultHash, &c.ErrorCode)
	if errors.Is(err, sql.ErrNoRows) {
		return Command{}, ErrNotFound
	}
	if err != nil {
		return Command{}, err
	}
	c.Payload = payload
	c.ResultPayload = result
	c.Status = Status(status)
	c.ExpiresAt, _ = time.Parse(time.RFC3339Nano, exp)
	return c, nil
}

// LatestResult returns the most recent device-authenticated diagnostic/result
// body for a sensor. The body is returned exactly as persisted; no backend
// serializer or scientific inference is applied.
func (s *Store) LatestResult(ctx context.Context, deviceID, sensorID string) (json.RawMessage, error) {
	if s == nil || s.db == nil || strings.TrimSpace(deviceID) == "" || strings.TrimSpace(sensorID) == "" {
		return nil, ErrInvalid
	}
	var payload []byte
	err := s.db.QueryRowContext(ctx, `
		SELECT result_payload FROM device_control_commands
		WHERE device_id = ? AND sensor_id = ? AND result_payload IS NOT NULL
		  AND length(result_payload) > 0
		ORDER BY updated_at DESC, command_id DESC LIMIT 1`, deviceID, sensorID).Scan(&payload)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return append(json.RawMessage(nil), payload...), nil
}

func (s *Store) Cancel(ctx context.Context, deviceID, commandID string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := s.now().UTC().Format(time.RFC3339Nano)
	result, err := tx.ExecContext(ctx, `UPDATE device_control_commands SET status='CANCELLED',updated_at=? WHERE device_id=? AND command_id=? AND status IN ('QUEUED','DELIVERED','ACCEPTED','CAPTURING','VALIDATED','PREVIEW_READY')`, now, deviceID, commandID)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return err
	}
	if changed == 0 {
		return ErrNotFound
	}
	if _, err = tx.ExecContext(ctx, `UPDATE device_control_sessions SET state='CLOSED',updated_at=? WHERE device_id=? AND session_id=(SELECT session_id FROM device_control_commands WHERE device_id=? AND command_id=?)`, now, deviceID, deviceID, commandID); err != nil {
		return err
	}
	return tx.Commit()
}

func (c Command) MarshalJSON() ([]byte, error) { type alias Command; return json.Marshal(alias(c)) }
func (s *Store) String() string                { return fmt.Sprintf("device-control lease=%s", s.lease) }
