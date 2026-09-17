package simulator

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
	"strings"
)

type Location struct {
	ID      string
	Name    string
	Address string
}

type Device struct {
	ID         string
	Code       string
	LocationID string
	Placement  string
	Token      string
}

type Slot struct {
	ID       string
	DeviceID string
	Stream   string
}

type Dataset struct {
	Locations []Location
	Devices   []Device
	Slots     []Slot
}

// Seed creates the small local-only dataset and reuses stable rows on every
// startup. It never deletes readings, decisions, or events.
func Seed(ctx context.Context, db *sql.DB, seed int64) (Dataset, error) {
	dataset := defaultDataset(seed)
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return Dataset{}, fmt.Errorf("begin simulation seed: %w", err)
	}
	defer tx.Rollback()
	for _, location := range dataset.Locations {
		if _, err := tx.ExecContext(ctx, `INSERT INTO locations (id, name, address_text) VALUES (?, ?, ?) ON CONFLICT(id) DO NOTHING`, location.ID, location.Name, location.Address); err != nil {
			return Dataset{}, fmt.Errorf("seed location %s: %w", location.ID, err)
		}
	}
	for _, device := range dataset.Devices {
		digest := sha256.Sum256([]byte(device.Token))
		if _, err := tx.ExecContext(ctx, `INSERT INTO devices (id, location_id, device_code, placement_label, credential_digest) VALUES (?, ?, ?, ?, ?) ON CONFLICT(id) DO NOTHING`, device.ID, device.LocationID, device.Code, device.Placement, hex.EncodeToString(digest[:])); err != nil {
			return Dataset{}, fmt.Errorf("seed device %s: %w", device.ID, err)
		}
	}
	for _, slot := range dataset.Slots {
		if _, err := tx.ExecContext(ctx, `INSERT INTO slots (id, device_id, waste_stream, operational_capacity) VALUES (?, ?, ?, ?) ON CONFLICT(id) DO NOTHING`, slot.ID, slot.DeviceID, slot.Stream, 20.0); err != nil {
			return Dataset{}, fmt.Errorf("seed slot %s: %w", slot.ID, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return Dataset{}, fmt.Errorf("commit simulation seed: %w", err)
	}
	return dataset, nil
}

func defaultDataset(seed int64) Dataset {
	locations := []Location{
		{ID: "sim-location-main", Name: "SIM - Gedung Utama", Address: "Area simulasi gedung utama"},
		{ID: "sim-location-canteen", Name: "SIM - Kantin", Address: "Area simulasi kantin"},
		{ID: "sim-location-lab", Name: "SIM - Laboratorium", Address: "Area simulasi laboratorium"},
	}
	devices := []Device{
		{ID: "sim-device-lobby", Code: "SIM-LOBBY-01", LocationID: "sim-location-main", Placement: "Lobi gedung utama", Token: simulationToken(seed, "SIM-LOBBY-01")},
		{ID: "sim-device-floor2", Code: "SIM-FLOOR2-01", LocationID: "sim-location-main", Placement: "Lantai 2 gedung utama", Token: simulationToken(seed, "SIM-FLOOR2-01")},
		{ID: "sim-device-canteen", Code: "SIM-CANTEEN-01", LocationID: "sim-location-canteen", Placement: "Pintu masuk kantin", Token: simulationToken(seed, "SIM-CANTEEN-01")},
		{ID: "sim-device-lab", Code: "SIM-LAB-01", LocationID: "sim-location-lab", Placement: "Koridor laboratorium", Token: simulationToken(seed, "SIM-LAB-01")},
	}
	slots := make([]Slot, 0, len(devices)*3)
	for _, device := range devices {
		for _, stream := range []string{"organic", "inorganic", "b3"} {
			slots = append(slots, Slot{ID: "sim-slot-" + strings.ToLower(strings.TrimPrefix(device.Code, "SIM-")) + "-" + stream, DeviceID: device.ID, Stream: stream})
		}
	}
	return Dataset{Locations: locations, Devices: devices, Slots: slots}
}

func simulationToken(seed int64, deviceCode string) string {
	return fmt.Sprintf("sigap-simulator-%d-%s", seed, strings.ToLower(deviceCode))
}
