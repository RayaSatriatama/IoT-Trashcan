package simulator

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
)

func TestSeedIsIdempotentAndCreatesExpectedDataset(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	first, err := Seed(context.Background(), db, 42)
	if err != nil {
		t.Fatal(err)
	}
	second, err := Seed(context.Background(), db, 42)
	if err != nil {
		t.Fatal(err)
	}
	if len(first.Locations) != 3 || len(first.Devices) != 4 || len(first.Slots) != 12 {
		t.Fatalf("dataset locations=%d devices=%d slots=%d", len(first.Locations), len(first.Devices), len(first.Slots))
	}
	if first.Devices[0].Token != second.Devices[0].Token {
		t.Fatal("seed token is not deterministic")
	}
	for _, table := range []string{"locations", "devices", "slots"} {
		var count int
		if err := db.QueryRow(`SELECT COUNT(*) FROM ` + table + ` WHERE id LIKE 'sim-%'`).Scan(&count); err != nil {
			t.Fatal(err)
		}
		want := map[string]int{"locations": 3, "devices": 4, "slots": 12}[table]
		if count != want {
			t.Fatalf("%s count=%d want=%d", table, count, want)
		}
	}
	var digest string
	if err := db.QueryRow(`SELECT credential_digest FROM devices WHERE id = ?`, first.Devices[0].ID).Scan(&digest); err != nil {
		t.Fatal(err)
	}
	if digest == first.Devices[0].Token || digest == "" {
		t.Fatalf("credential digest is not safely stored: %q", digest)
	}
}
