package simulator

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

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
	service := ingest.NewService(db, ingest.WithDecisionEngine(DemoEngine()), ingest.WithSafetyConfig(DemoSafetyConfig()))
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
	if readings != 12 || decisions != 12 {
		t.Fatalf("pipeline rows readings=%d decisions=%d", readings, decisions)
	}
	var status, version string
	if err := db.QueryRow(`SELECT fis_status, fis_version FROM decisions ORDER BY reading_id LIMIT 1`).Scan(&status, &version); err != nil {
		t.Fatal(err)
	}
	if status != "Normal" || version != demoFISVersion {
		t.Fatalf("decision status=%q version=%q", status, version)
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
	service := ingest.NewService(db, ingest.WithDecisionEngine(DemoEngine()), ingest.WithSafetyConfig(DemoSafetyConfig()))
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
