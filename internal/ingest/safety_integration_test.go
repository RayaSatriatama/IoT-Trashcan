package ingest

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/safety"
)

func TestIngestPersistsUnavailableSafetyStateWhenConfiguredInputIsMissing(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	service.safetyConfig = safety.Config{Thresholds: []safety.Threshold{{
		Signal: safety.SignalMQ7CO, AtOrAbove: 0.8, Reason: "test-only CO fixture",
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
		var state sql.NullString
		if err := rows.Scan(&state); err != nil {
			t.Fatal(err)
		}
		count++
		if count < 3 && state.Valid {
			t.Fatalf("non-B3 safety_state = %q, want NULL", state.String)
		}
		if count == 3 && (!state.Valid || state.String != safety.StateUnavailable) {
			t.Fatalf("B3 safety_state = %#v, want %q", state, safety.StateUnavailable)
		}
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

func TestIngestDoesNotTreatRawMQADCAsNormalizedSafetyEvidence(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	raw := 4095.0
	req := decisionRequest()
	req.Slots[2].Sensors = []SensorReading{{SensorID: "mq7-b3", SensorType: "mq7", Parameter: "co", RawValue: &raw, RawUnit: "ADC", SensorState: SensorValid}}
	service.safetyConfig = safety.Config{Thresholds: []safety.Threshold{{Signal: safety.SignalMQ7CO, AtOrAbove: 0.8, Reason: "test-only CO fixture"}}}

	if _, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"safety":"raw-only"}`)); err != nil {
		t.Fatal(err)
	}
	var state string
	if err := db.QueryRow(`SELECT safety_state FROM decisions WHERE reading_id = 3`).Scan(&state); err != nil {
		t.Fatal(err)
	}
	if state != safety.StateUnavailable {
		t.Fatalf("safety_state=%q, want Unavailable for raw-only MQ evidence", state)
	}
}

func TestIngestPreservesExplicitLegacySafetySignals(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	gas := 90.0
	req := decisionRequest()
	req.Slots[2].GasRaw = &gas
	service.safetyConfig = safety.Config{Thresholds: []safety.Threshold{{Signal: safety.SignalGasRaw, AtOrAbove: 80, Reason: "legacy gas fixture"}}}

	if _, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"safety":"legacy"}`)); err != nil {
		t.Fatal(err)
	}
	var state string
	if err := db.QueryRow(`SELECT safety_state FROM decisions WHERE reading_id = 3`).Scan(&state); err != nil {
		t.Fatal(err)
	}
	if state != safety.StateSafetyAlert {
		t.Fatalf("safety_state=%q, want Safety Alert", state)
	}
}

func TestIngestPersistsSafetyEvidenceWithoutDecisionEngine(t *testing.T) {
	db, service := decisionTestService(t, nil)
	defer db.Close()
	co := 0.9
	req := decisionRequest()
	req.Slots[2].Sensors = []SensorReading{{SensorID: "mq7-b3", SensorType: "mq7", Parameter: "co", NormalizedValue: &co, SensorState: SensorValid}}
	service.safetyConfig = safety.Config{Thresholds: []safety.Threshold{{Signal: safety.SignalMQ7CO, AtOrAbove: 0.8, Reason: "test-only CO fixture"}}}

	if _, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"safety":"without-fis"}`)); err != nil {
		t.Fatal(err)
	}
	var decisions int
	var status sql.NullString
	var state string
	if err := db.QueryRow(`SELECT COUNT(*), fis_status, safety_state FROM decisions`).Scan(&decisions, &status, &state); err != nil {
		t.Fatal(err)
	}
	if decisions != 1 || status.Valid || state != safety.StateSafetyAlert {
		t.Fatalf("decisions=%d status=%#v safety=%q", decisions, status, state)
	}
}
