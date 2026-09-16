// Package ingest owns the authenticated, transactional device-reading write path.
package ingest

import (
	"encoding/json"
	"errors"
	"time"
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
	Stream      string   `json:"stream"`
	Fill        *float64 `json:"fill"`
	Weight      *float64 `json:"weight"`
	GasRaw      *float64 `json:"gas_raw"`
	Temperature *float64 `json:"temperature"`
}

// Result is returned only after all readings and the heartbeat commit.
type Result struct {
	DeviceID       string    `json:"device_id"`
	RecordedAt     time.Time `json:"recorded_at"`
	ReadingsStored int       `json:"readings_stored"`
}
