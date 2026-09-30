package simulator

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
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
	if len(first.Locations) != 8 || len(first.Devices) != 8 || len(first.Slots) != 24 {
		t.Fatalf("dataset locations=%d devices=%d slots=%d", len(first.Locations), len(first.Devices), len(first.Slots))
	}
	wantLocations := []string{"Gedung A / Biru", "Gedung B", "Gedung E", "Lapangan Olahraga", "Parkiran Motor", "Parkiran Mobil", "Masjid As-Sakinah", "Asrama"}
	for index, wantName := range wantLocations {
		location := first.Locations[index]
		if location.Name != wantName || location.Address != "" {
			t.Fatalf("location[%d]=%+v want name=%q with empty address", index, location, wantName)
		}
	}
	wantDeviceCodes := []string{"SIGAP-GA-01", "SIGAP-GB-01", "SIGAP-GE-01", "SIGAP-LO-01", "SIGAP-PM-01", "SIGAP-PB-01", "SIGAP-MAS-01", "SIGAP-ASR-01"}
	for index, wantCode := range wantDeviceCodes {
		device := first.Devices[index]
		if device.Code != wantCode || device.Placement != "" {
			t.Fatalf("device[%d]=%+v want code=%q with empty placement", index, device, wantCode)
		}
	}
	if first.Devices[0].Token != second.Devices[0].Token {
		t.Fatal("seed token is not deterministic")
	}
	for _, table := range []string{"locations", "devices", "slots"} {
		var count int
		if err := db.QueryRow(`SELECT COUNT(*) FROM ` + table + ` WHERE id LIKE 'sim-%'`).Scan(&count); err != nil {
			t.Fatal(err)
		}
		want := map[string]int{"locations": 8, "devices": 8, "slots": 24}[table]
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

func TestSeedRefreshesCredentialDigestWhenSeedChanges(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	first, err := Seed(context.Background(), db, 42)
	if err != nil {
		t.Fatal(err)
	}
	second, err := Seed(context.Background(), db, 99)
	if err != nil {
		t.Fatal(err)
	}
	if first.Devices[0].Token == second.Devices[0].Token {
		t.Fatal("changing seed did not rotate deterministic simulator credential")
	}

	var digest string
	if err := db.QueryRow(`SELECT credential_digest FROM devices WHERE id = ?`, second.Devices[0].ID).Scan(&digest); err != nil {
		t.Fatal(err)
	}
	want := sha256.Sum256([]byte(second.Devices[0].Token))
	if digest != hex.EncodeToString(want[:]) {
		t.Fatalf("credential digest=%q want refreshed digest", digest)
	}

	var devices int
	if err := db.QueryRow(`SELECT COUNT(*) FROM devices WHERE id LIKE 'sim-%'`).Scan(&devices); err != nil {
		t.Fatal(err)
	}
	if devices != 8 {
		t.Fatalf("devices=%d want=8 after reseed", devices)
	}
}

func TestDefaultDatasetRetainsLegacySlotIDsForRenamedDevices(t *testing.T) {
	dataset := defaultDataset(42)
	ids := make(map[string]bool, len(dataset.Slots))
	for _, slot := range dataset.Slots {
		ids[slot.ID] = true
	}
	for _, wantID := range []string{
		"sim-slot-lobby-01-organic",
		"sim-slot-floor2-01-organic",
		"sim-slot-canteen-01-organic",
		"sim-slot-lab-01-organic",
	} {
		if !ids[wantID] {
			t.Fatalf("missing stable slot id %q", wantID)
		}
	}
}
