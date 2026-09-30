package simulator

import (
	"context"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

func TestPayloadProvidesTruthfulPerStreamSensorInventory(t *testing.T) {
	request, send := payloadFor(Device{ID: "sim-device-canteen", Code: "SIM-CANTEEN-01"}, PhaseNormal, "2026-09-22T12:00:00Z", nil)
	if !send || len(request.Slots) != 3 {
		t.Fatalf("payload send=%v slots=%d", send, len(request.Slots))
	}
	want := map[string][]string{
		"organic":   {"fill", "hx711", "mq135", "mq4"},
		"inorganic": {"fill", "hx711", "mq135"},
		"b3":        {"fill", "hx711", "mq135", "mq7", "mq8", "ds18b20"},
	}
	for _, slot := range request.Slots {
		if slot.OGI == nil || slot.AccumulationHours == nil {
			t.Fatalf("%s missing source-backed simulation inputs", slot.Stream)
		}
		got := make([]string, 0, len(slot.Sensors))
		for _, sensor := range slot.Sensors {
			got = append(got, sensor.SensorType)
			if strings.EqualFold(sensor.Unit, "ppm") {
				t.Fatalf("%s sensor %s invented ppm", slot.Stream, sensor.SensorID)
			}
			if sensor.RawValue != nil && sensor.RawUnit == "" {
				t.Fatalf("%s sensor %s missing explicit raw unit", slot.Stream, sensor.SensorID)
			}
		}
		if strings.Join(got, ",") != strings.Join(want[slot.Stream], ",") {
			t.Fatalf("%s sensors=%v want=%v", slot.Stream, got, want[slot.Stream])
		}
	}
}

func TestTickUsesRealIngestPipeline(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	dataset, err := Seed(context.Background(), db, 42)
	if err != nil {
		t.Fatal(err)
	}
	service := ingest.NewService(db, ingest.WithDecisionProfiles(DemoEngines()), ingest.WithSafetyConfig(DemoSafetyConfig()))
	simulator := New(service, dataset, Config{Enabled: true, Interval: time.Second, Seed: 42, Clock: func() time.Time {
		return time.Date(2026, 9, 17, 12, 0, 0, 0, time.UTC)
	}})
	if err := simulator.Tick(context.Background()); err != nil {
		t.Fatal(err)
	}
	var readings, decisions int
	if err := db.QueryRow(`SELECT COUNT(*) FROM readings`).Scan(&readings); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM decisions`).Scan(&decisions); err != nil {
		t.Fatal(err)
	}
	if readings != 24 || decisions != 24 {
		t.Fatalf("pipeline rows readings=%d decisions=%d", readings, decisions)
	}
	var status, version, trace string
	if err := db.QueryRow(`SELECT fis_status, fis_version, trace_json FROM decisions ORDER BY reading_id LIMIT 1`).Scan(&status, &version, &trace); err != nil {
		t.Fatal(err)
	}
	if status != "Normal" || version != demoFISVersion {
		t.Fatalf("decision status=%q version=%q", status, version)
	}
	for _, input := range []string{"fill", "weight_utilization", "ogi", "accumulation_hours"} {
		if !strings.Contains(trace, `"`+input+`"`) {
			t.Fatalf("trace lost %s input: %s", input, trace)
		}
	}
	var versions int
	if err := db.QueryRow(`SELECT COUNT(DISTINCT fis_version) FROM decisions`).Scan(&versions); err != nil {
		t.Fatal(err)
	}
	if versions != 3 {
		t.Fatalf("distinct FIS versions=%d, want three stream profiles", versions)
	}
	var nonB3Safety int
	if err := db.QueryRow(`SELECT COUNT(*) FROM decisions d JOIN readings r ON r.id=d.reading_id JOIN slots s ON s.id=r.slot_id WHERE s.waste_stream != 'b3' AND d.safety_state IS NOT NULL`).Scan(&nonB3Safety); err != nil {
		t.Fatal(err)
	}
	if nonB3Safety != 0 {
		t.Fatalf("non-B3 safety decisions=%d, want none", nonB3Safety)
	}
}

func TestInvalidSimulationPhasePersistsDataInvalidWithoutFIS(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	dataset, err := Seed(context.Background(), db, 42)
	if err != nil {
		t.Fatal(err)
	}
	service := ingest.NewService(db, ingest.WithDecisionProfiles(DemoEngines()), ingest.WithSafetyConfig(DemoSafetyConfig()))
	clock := time.Date(2026, 9, 17, 12, 0, 0, 0, time.UTC)
	simulator := New(service, dataset, Config{Enabled: true, Interval: time.Second, Seed: 42, Clock: func() time.Time { return clock }})
	for tick := 0; tick < 5; tick++ {
		if err := simulator.Tick(context.Background()); err != nil {
			t.Fatal(err)
		}
		clock = clock.Add(time.Second)
	}
	var invalid, invalidScored, scored int
	if err := db.QueryRow(`SELECT COUNT(*) FROM decisions WHERE fis_status = 'Data Invalid'`).Scan(&invalid); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM decisions WHERE fis_status = 'Data Invalid' AND fis_score IS NOT NULL`).Scan(&invalidScored); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM decisions WHERE fis_score IS NOT NULL`).Scan(&scored); err != nil {
		t.Fatal(err)
	}
	if invalid == 0 || invalidScored != 0 || scored == 0 {
		t.Fatalf("invalid=%d invalidScored=%d scored=%d", invalid, invalidScored, scored)
	}
}
