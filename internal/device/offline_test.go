package device

import (
	"errors"
	"testing"
	"time"
)

func TestIsOfflineRejectsMissingThreshold(t *testing.T) {
	_, err := IsOffline(nil, time.Now(), OfflineConfig{})
	if !errors.Is(err, ErrInvalidOfflineAfter) {
		t.Fatalf("IsOffline() error = %v, want %v", err, ErrInvalidOfflineAfter)
	}
}

func TestIsOfflineDistinguishesFreshAndStaleDevices(t *testing.T) {
	now := time.Date(2026, time.September, 16, 12, 0, 0, 0, time.UTC)
	config := OfflineConfig{OfflineAfter: 10 * time.Minute}
	fresh := now.Add(-9 * time.Minute)
	stale := now.Add(-10 * time.Minute)

	if offline, err := IsOffline(&fresh, now, config); err != nil || offline {
		t.Fatalf("fresh device offline=%t err=%v, want false nil", offline, err)
	}
	if offline, err := IsOffline(&stale, now, config); err != nil || !offline {
		t.Fatalf("stale device offline=%t err=%v, want true nil", offline, err)
	}
	if offline, err := IsOffline(nil, now, config); err != nil || !offline {
		t.Fatalf("never-seen device offline=%t err=%v, want true nil", offline, err)
	}
}
