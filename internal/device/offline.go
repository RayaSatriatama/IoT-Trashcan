package device

import (
	"errors"
	"time"
)

var ErrInvalidOfflineAfter = errors.New("offline duration must be positive")

// OfflineConfig makes the offline threshold explicit at the composition root.
// It intentionally has no production default because no authoritative duration
// is defined by the repository's domain material.
type OfflineConfig struct {
	OfflineAfter time.Duration
}

// IsOffline derives device availability from the server-side heartbeat. A
// device that has never checked in is offline. A positive threshold is
// required so callers cannot silently rely on an invented duration.
func IsOffline(lastSeen *time.Time, now time.Time, config OfflineConfig) (bool, error) {
	if config.OfflineAfter <= 0 {
		return false, ErrInvalidOfflineAfter
	}
	if lastSeen == nil {
		return true, nil
	}
	return !now.Before(lastSeen.Add(config.OfflineAfter)), nil
}
