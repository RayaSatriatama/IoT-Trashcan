package ingest

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/safety"
)

func TestIngestPersistsUnavailableSafetyStateWhenConfiguredInputIsMissing(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	service.safetyConfig = safety.Config{Thresholds: []safety.Threshold{{
		Signal: safety.SignalGasRaw, AtOrAbove: 50, Reason: "test-only gas fixture",
	}}}

	if _, err := service.Ingest(context.Background(), "fixture-token", decisionRequest(), []byte(`{"safety":"missing"}`)); err != nil {
		t.Fatal(err)
	}

	rows, err := db.Query(`SELECT safety_state FROM decisions ORDER BY reading_id`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	count := 0
	for rows.Next() {
		var state string
		if err := rows.Scan(&state); err != nil {
			t.Fatal(err)
		}
		if state != safety.StateUnavailable {
			t.Fatalf("safety_state = %q, want %q", state, safety.StateUnavailable)
		}
		count++
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	if count != 3 {
		t.Fatalf("decision count = %d, want 3", count)
	}
}

func TestIngestRejectsInvalidSafetyConfigurationAtomically(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	service.safetyConfig = safety.Config{Thresholds: []safety.Threshold{{
		Signal: safety.Signal("typo"), AtOrAbove: 50, Reason: "invalid signal fixture",
	}}}

	_, err := service.Ingest(context.Background(), "fixture-token", decisionRequest(), []byte(`{"safety":"invalid-config"}`))
	if !errors.Is(err, ErrPersistence) || !strings.Contains(err.Error(), "evaluate safety") {
		t.Fatalf("Ingest() error = %v, want ErrPersistence from safety evaluation", err)
	}
	assertCount(t, db, "readings", 0)
	assertCount(t, db, "decisions", 0)

	var lastSeen string
	if err := db.QueryRow(`SELECT last_seen FROM devices WHERE id = 'device-1'`).Scan(&lastSeen); err != nil {
		t.Fatal(err)
	}
	if lastSeen != "before-ingest" {
		t.Fatalf("last_seen = %q, want before-ingest", lastSeen)
	}
}
