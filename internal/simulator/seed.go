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
	SlotKey    string
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
// startup. It never deletes readings, decisions, or events. Simulator-owned
// metadata is refreshed on conflict so changing the simulation seed also
// rotates the deterministic local credential digest without duplicating rows.
func Seed(ctx context.Context, db *sql.DB, seed int64) (Dataset, error) {
	dataset := defaultDataset(seed)
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return Dataset{}, fmt.Errorf("begin simulation seed: %w", err)
	}
	defer tx.Rollback()
	for _, location := range dataset.Locations {
		if _, err := tx.ExecContext(ctx, `INSERT INTO locations (id, name, address_text) VALUES (?, ?, ?) ON CONFLICT(id) DO UPDATE SET name = excluded.name, address_text = excluded.address_text`, location.ID, location.Name, location.Address); err != nil {
			return Dataset{}, fmt.Errorf("seed location %s: %w", location.ID, err)
		}
	}
	for _, device := range dataset.Devices {
		digest := sha256.Sum256([]byte(device.Token))
		if _, err := tx.ExecContext(ctx, `INSERT INTO devices (id, location_id, device_code, placement_label, credential_digest) VALUES (?, ?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET location_id = excluded.location_id, device_code = excluded.device_code, placement_label = excluded.placement_label, credential_digest = excluded.credential_digest`, device.ID, device.LocationID, device.Code, device.Placement, hex.EncodeToString(digest[:])); err != nil {
			return Dataset{}, fmt.Errorf("seed device %s: %w", device.ID, err)
		}
	}
	for _, slot := range dataset.Slots {
		if _, err := tx.ExecContext(ctx, `INSERT INTO slots (id, device_id, waste_stream, operational_capacity) VALUES (?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET device_id = excluded.device_id, waste_stream = excluded.waste_stream, operational_capacity = excluded.operational_capacity`, slot.ID, slot.DeviceID, slot.Stream, 20.0); err != nil {
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
		{ID: "sim-location-main", Name: "Gedung A / Biru"},
		{ID: "sim-location-canteen", Name: "Gedung B"},
		{ID: "sim-location-lab", Name: "Gedung E"},
		{ID: "sim-location-sports", Name: "Lapangan Olahraga"},
		{ID: "sim-location-motor", Name: "Parkiran Motor"},
		{ID: "sim-location-mobil", Name: "Parkiran Mobil"},
		{ID: "sim-location-masjid", Name: "Masjid As-Sakinah"},
		{ID: "sim-location-asrama", Name: "Asrama"},
	}
	devices := []Device{
		{ID: "sim-device-lobby", Code: "SIGAP-GA-01", LocationID: "sim-location-main", SlotKey: "lobby-01", Token: simulationToken(seed, "SIGAP-GA-01")},
		{ID: "sim-device-floor2", Code: "SIGAP-GB-01", LocationID: "sim-location-canteen", SlotKey: "floor2-01", Token: simulationToken(seed, "SIGAP-GB-01")},
		{ID: "sim-device-canteen", Code: "SIGAP-GE-01", LocationID: "sim-location-lab", SlotKey: "canteen-01", Token: simulationToken(seed, "SIGAP-GE-01")},
		{ID: "sim-device-lab", Code: "SIGAP-LO-01", LocationID: "sim-location-sports", SlotKey: "lab-01", Token: simulationToken(seed, "SIGAP-LO-01")},
		{ID: "sim-device-motor", Code: "SIGAP-PM-01", LocationID: "sim-location-motor", Token: simulationToken(seed, "SIGAP-PM-01")},
		{ID: "sim-device-mobil", Code: "SIGAP-PB-01", LocationID: "sim-location-mobil", Token: simulationToken(seed, "SIGAP-PB-01")},
		{ID: "sim-device-masjid", Code: "SIGAP-MAS-01", LocationID: "sim-location-masjid", Token: simulationToken(seed, "SIGAP-MAS-01")},
		{ID: "sim-device-asrama", Code: "SIGAP-ASR-01", LocationID: "sim-location-asrama", Token: simulationToken(seed, "SIGAP-ASR-01")},
	}
	slots := make([]Slot, 0, len(devices)*3)
	for _, device := range devices {
		slotKey := device.SlotKey
		if slotKey == "" {
			slotKey = strings.ToLower(strings.TrimPrefix(device.Code, "SIGAP-"))
		}
		for _, stream := range []string{"organic", "inorganic", "b3"} {
			slots = append(slots, Slot{ID: "sim-slot-" + slotKey + "-" + stream, DeviceID: device.ID, Stream: stream})
		}
	}
	return Dataset{Locations: locations, Devices: devices, Slots: slots}
}

func simulationToken(seed int64, deviceCode string) string {
	return fmt.Sprintf("sigap-simulator-%d-%s", seed, strings.ToLower(deviceCode))
}
