package devicecontrol

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
)

const normalizedMQ135Criteria = `{"response_mode":"normalized","samples":3,"min_duration_ms":1000,"max_age_ms":250,"max_variation_milli_percent":5000,"circuit_mv":5000,"divider_top_ohm":10000,"divider_bottom_ohm":15000,"topology":1,"hardware_confirmed":1,"reference_confirmed":1,"hardware_identity":"SYNTHETIC-HW","reference_environment":"LAB-01","reference_kind":"operational","preparation_confirmed":0,"preparation_method":"unknown","heating_duration_us":0}`

func TestMQ135NormalizedCriteriaForwardExactContractAndOptionalReferences(t *testing.T) {
	for name, payload := range map[string]string{
		"required only":    normalizedMQ135Criteria,
		"temperature only": normalizedMQ135Criteria[:len(normalizedMQ135Criteria)-1] + `,"reference_temperature_centi_c":-500}`,
		"humidity only":    normalizedMQ135Criteria[:len(normalizedMQ135Criteria)-1] + `,"reference_humidity_centi_pct":5000}`,
	} {
		t.Run(name, func(t *testing.T) {
			encoded, err := mq135CommandPayload("set_criteria", []byte(payload))
			if err != nil {
				t.Fatal(err)
			}
			var fields map[string]any
			if err := json.Unmarshal(encoded, &fields); err != nil {
				t.Fatal(err)
			}
			if fields["response_mode"] != "normalized" || fields["reference_kind"] != "operational" || fields["preparation_method"] != "unknown" || fields["module_load_ohm"] != nil {
				t.Fatalf("criteria not forwarded as normalized contract: %s", encoded)
			}
			want := 17
			if name != "required only" {
				want++
			}
			if len(fields) != want {
				t.Fatalf("forwarded %d fields, want %d: %s", len(fields), want, encoded)
			}
		})
	}
}

func TestMQ135NormalizedCriteriaRejectBeforeStoreMutation(t *testing.T) {
	store, db, now := testStore(t)
	start := commandRequest(now)
	start.SensorID, start.Payload = "mq135_gas", []byte(`{}`)
	if _, err := store.Enqueue(context.Background(), start); err != nil {
		t.Fatal(err)
	}
	for name, payload := range map[string]string{
		"legacy module load mixed into normalized": normalizedMQ135Criteria[:len(normalizedMQ135Criteria)-1] + `,"module_load_ohm":10000}`,
		"caller q0 rejected":                       normalizedMQ135Criteria[:len(normalizedMQ135Criteria)-1] + `,"q0":1}`,
		"unknown key rejected":                     normalizedMQ135Criteria[:len(normalizedMQ135Criteria)-1] + `,"extra":1}`,
		"mode required":                            `{"reference_kind":"operational"}`,
		"duplicate key":                            `{"response_mode":"normalized","response_mode":"normalized"}`,
		"wrong mode":                               `{"response_mode":"legacy"}`,
	} {
		t.Run(name, func(t *testing.T) {
			req := start
			req.ID = "invalid-" + name
			req.Operation, req.Payload = "set_criteria", []byte(payload)
			if _, err := store.Enqueue(context.Background(), req); !errors.Is(err, ErrInvalid) {
				t.Fatalf("invalid criteria accepted: %v", err)
			}
			var count int
			if err := db.QueryRow(`SELECT count(*) FROM device_control_commands`).Scan(&count); err != nil {
				t.Fatal(err)
			}
			if count != 1 {
				t.Fatalf("invalid criteria mutated command store: count=%d", count)
			}
		})
	}
}
