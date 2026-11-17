package devicecontrol

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"testing"
)

func mq135Fixture(t *testing.T) []byte {
	t.Helper()
	data, err := os.ReadFile("../../contracts/calibration/mq135-relative/v1/fixtures/ready-test-only.json")
	if err != nil {
		t.Fatal(err)
	}
	return data
}

const mq135ReferencePayload = `{"parameters":{"samples":3,"min_duration_ms":2,"max_age_ms":1,"max_variation_milli_percent":10,"circuit_mv":5000,"module_load_ohm":10000,"divider_top_ohm":10000,"divider_bottom_ohm":15000,"topology":1,"hardware_confirmed":1,"preparation_confirmed":1,"reference_confirmed":1,"reference_temperature_centi_c":2500,"reference_humidity_centi_pct":5000,"heating_duration_us":10000,"hardware_identity":"SYNTHETIC","reference_environment":"SYNTHETIC"}}`

func TestMq135ReferenceParametersAreFlattenedAndStrict(t *testing.T) {
	s, _, now := testStore(t)
	ctx := context.Background()
	start := commandRequest(now)
	start.SensorID, start.Payload = "mq135_gas", []byte(`{}`)
	if _, err := s.Enqueue(ctx, start); err != nil {
		t.Fatal(err)
	}
	req := start
	req.ID, req.Operation, req.Payload = "gas-criteria", "set_criteria", []byte(mq135ReferencePayload)
	got, err := s.Enqueue(ctx, req)
	if err != nil {
		t.Fatal(err)
	}
	var flat map[string]any
	if err := json.Unmarshal(got.Payload, &flat); err != nil {
		t.Fatal(err)
	}
	if len(flat) != 17 || flat["hardware_identity"] != "SYNTHETIC" || flat["samples"] != float64(3) {
		t.Fatal("device criteria are not flat and complete")
	}
	for _, invalid := range []string{
		`{"hardware_confirmed":0}`,
		`{"parameters":{},"adc_profile":{"reference_mv":3300}}`,
		`{"parameters":{"samples":3},"samples":4}`,
		`{"parameters":{"samples":3,"samples":4}}`,
		`{"r0_ohm":1}`,
	} {
		req.ID, req.Payload = "invalid-criteria", []byte(invalid)
		if _, err := s.Enqueue(ctx, req); !errors.Is(err, ErrInvalid) {
			t.Fatalf("invalid criteria accepted: %s: %v", invalid, err)
		}
	}
}

func TestMq135EvidencePersistsExactDeviceResultWithoutTelemetryMutation(t *testing.T) {
	s, db, now := testStore(t)
	req := commandRequest(now)
	req.SensorID, req.Operation, req.SessionID, req.Payload = "mq135_gas", "evidence", "", []byte(`{}`)
	ctx := context.Background()
	if _, err := s.Enqueue(ctx, req); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Claim(ctx, req.DeviceID); err != nil {
		t.Fatal(err)
	}
	fixture := mq135Fixture(t)
	result := ResultRequest{Status: Accepted, Payload: fixture}
	if _, err := s.UpdateResult(ctx, req.DeviceID, req.ID, result); err != nil {
		t.Fatal(err)
	}
	got, err := s.Get(ctx, req.DeviceID, req.ID)
	if err != nil || !bytes.Equal(got.ResultPayload, fixture) {
		t.Fatalf("device result changed: %v", err)
	}
	var count int
	if err := db.QueryRow(`SELECT count(*) FROM telemetry_records`).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 0 {
		t.Fatal("control evidence changed telemetry-v3")
	}
	var bad map[string]any
	if err := json.Unmarshal(fixture, &bad); err != nil {
		t.Fatal(err)
	}
	bad["nh3_ppm"] = 42
	badBytes, _ := json.Marshal(bad)
	if _, err := s.UpdateResult(ctx, req.DeviceID, req.ID, ResultRequest{Status: Accepted, Payload: badBytes}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("unvalidated NH3 accepted: %v", err)
	}
}
