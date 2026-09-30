// Package ingest owns the authenticated, transactional device-reading write path.
package ingest

import (
	"encoding/json"
	"errors"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/fuzzy"
)

var (
	// ErrInvalidRequest means the payload does not satisfy the Task 01 structural contract.
	ErrInvalidRequest = errors.New("invalid ingest request")
	// ErrUnauthorized intentionally covers missing, invalid, and device-mismatched credentials.
	ErrUnauthorized = errors.New("unauthorized device credential")
	// ErrNotFound means a referenced device or its required slots are unavailable.
	ErrNotFound = errors.New("ingest resource not found")
	// ErrPersistence means a database operation could not be completed.
	ErrPersistence = errors.New("ingest persistence failed")
	// ErrConflict means a device submitted a different payload for a timestamp
	// that has already been persisted.
	ErrConflict = errors.New("conflicting ingest replay")
	// ErrInvalidCapacity distinguishes a missing capacity from a broken
	// operational-capacity configuration.
	ErrInvalidCapacity = errors.New("invalid operational capacity")
)

// Request is the stable device-ingestion payload. Numeric pointers retain the
// difference between an omitted sensor field and a measured zero.
type Request struct {
	DeviceID   string          `json:"device_id"`
	RecordedAt string          `json:"recorded_at"`
	Slots      []Slot          `json:"slots"`
	Health     json.RawMessage `json:"health"`
}

// Slot contains unprocessed, device-reported sensor values for one waste stream.
type Slot struct {
	Stream            string          `json:"stream"`
	Fill              *float64        `json:"fill"`
	Weight            *float64        `json:"weight"`
	GasRaw            *float64        `json:"gas_raw"`
	Temperature       *float64        `json:"temperature"`
	OGI               *float64        `json:"ogi"`
	AccumulationHours *float64        `json:"accumulation_hours"`
	Sensors           []SensorReading `json:"sensors,omitempty"`
}

type SensorState string

const (
	SensorValid     SensorState = "VALID"
	SensorWarmingUp SensorState = "WARMING_UP"
	SensorStale     SensorState = "STALE"
	SensorInvalid   SensorState = "INVALID"
	SensorDisabled  SensorState = "DISABLED"
)

// SensorReading is an optional physical-sensor record. Numeric pointers keep
// unavailable values distinct from a measured zero.
type SensorReading struct {
	SensorID           string      `json:"sensor_id"`
	SensorType         string      `json:"sensor_type"`
	Parameter          string      `json:"parameter"`
	RawValue           *float64    `json:"raw_value"`
	EngineeringValue   *float64    `json:"engineering_value"`
	NormalizedValue    *float64    `json:"normalized_value"`
	RawUnit            string      `json:"raw_unit,omitempty"`
	Unit               string      `json:"unit"`
	SensorState        SensorState `json:"sensor_state"`
	CalibrationVersion string      `json:"calibration_version,omitempty"`
	RecordedAt         string      `json:"recorded_at,omitempty"`
}

// Result is returned only after all readings and the heartbeat commit.
type Result struct {
	DeviceID       string    `json:"device_id"`
	RecordedAt     time.Time `json:"recorded_at"`
	ReadingsStored int       `json:"readings_stored"`
	Deduplicated   bool      `json:"deduplicated"`
}

// ValidationResult captures operational sensor validity without losing the
// original raw reading. Structural request errors still use ErrInvalidRequest.
type ValidationResult struct {
	Valid   bool
	Reasons []string
}

// NormalizedReading contains only values whose semantics are currently
// source-backed. Nil represents unavailable data, never a fabricated zero.
type NormalizedReading struct {
	FillPct           *float64
	WeightUtilization *float64
	OGI               *float64
	AccumulationHours *float64
	Temperature       *float64
}

// DecisionTrace is persisted with a decision so research views never
// reconstruct historical evidence using a newer FIS configuration.
type DecisionTrace struct {
	Inputs         map[string]float64            `json:"inputs"`
	Memberships    map[string]map[string]float64 `json:"memberships"`
	Rules          []fuzzy.RuleResult            `json:"rules"`
	TriggeredRules int                           `json:"triggered_rules"`
	Score          *float64                      `json:"score"`
	Status         string                        `json:"status"`
	Version        string                        `json:"version"`
	DominantReason string                        `json:"dominant_reason"`
}

type SafetyChannelEvidence struct {
	SensorID    string      `json:"sensor_id"`
	SensorType  string      `json:"sensor_type"`
	Parameter   string      `json:"parameter"`
	Value       *float64    `json:"value"`
	Normalized  *float64    `json:"normalized"`
	Unit        string      `json:"unit"`
	SensorState SensorState `json:"sensor_state"`
	RecordedAt  string      `json:"recorded_at,omitempty"`
}

type SafetyEvidence struct {
	State    string                  `json:"state"`
	Reason   string                  `json:"reason,omitempty"`
	Channels []SafetyChannelEvidence `json:"channels"`
}
