package history

import (
	"context"
	"database/sql"
	"encoding/csv"
	"net/url"
	"path/filepath"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
)

func TestParseFiltersValidatesRFC3339RangeAndStatus(t *testing.T) {
	filters, err := ParseFilters(url.Values{
		"from":   []string{"2026-09-01T00:00:00Z"},
		"to":     []string{"2026-09-02T00:00:00Z"},
		"status": []string{"Waspada"},
	})
	if err != nil {
		t.Fatalf("ParseFilters() error = %v", err)
	}
	if filters.From == nil || filters.To == nil || filters.Status != "Waspada" {
		t.Fatalf("filters = %+v", filters)
	}

	cases := []url.Values{
		{"from": []string{"not-a-date"}},
		{"to": []string{"not-a-date"}},
		{"from": []string{"2026-09-02T00:00:00Z"}, "to": []string{"2026-09-01T00:00:00Z"}},
		{"status": []string{"Not A Status"}},
	}
	for _, values := range cases {
		if _, err := ParseFilters(values); err == nil {
			t.Errorf("ParseFilters(%v) returned nil error", values)
		}
	}
}

func TestQueryHistoryNewestFirstAndCombinedFilters(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	items, err := Query(context.Background(), db, Filters{LocationID: "loc-a", DeviceID: "dev-a", Status: "Safety Alert"})
	if err != nil {
		t.Fatalf("Query() error = %v", err)
	}
	if len(items) != 1 {
		t.Fatalf("len(items) = %d, want 1", len(items))
	}
	item := items[0]
	if item.ReadingID != 3 || item.Stream != "b3" || item.GasRaw == nil || *item.GasRaw != 99 {
		t.Fatalf("item = %+v", item)
	}

	all, err := Query(context.Background(), db, Filters{})
	if err != nil {
		t.Fatalf("Query() error = %v", err)
	}
	if len(all) != 3 || all[0].ReadingID != 3 || all[1].ReadingID != 2 || all[2].ReadingID != 1 {
		t.Fatalf("newest-first items = %+v", all)
	}
	if all[1].Fill == nil || *all[1].Fill != 0 {
		t.Fatalf("zero fill was not preserved: %+v", all[1].Fill)
	}
	if all[2].WeightUtilization == nil || *all[2].WeightUtilization != 50 {
		t.Fatalf("normalized weight = %+v", all[2].WeightUtilization)
	}
	if all[1].OGI != nil || all[1].AccumulationHours != nil {
		t.Fatalf("missing normalized values must remain nil: %+v", all[1])
	}
}

func TestQueryHistoryDateFiltersAreHalfOpen(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()
	filters, err := ParseFilters(url.Values{
		"from": []string{"2026-09-17T10:00:00Z"},
		"to":   []string{"2026-09-17T10:00:02Z"},
	})
	if err != nil {
		t.Fatal(err)
	}
	items, err := Query(context.Background(), db, filters)
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 2 || items[0].ReadingID != 2 || items[1].ReadingID != 1 {
		t.Fatalf("date-filtered items = %+v", items)
	}
}

func TestWriteCSVUsesStableHeaderEscapesValuesAndLeavesNullEmpty(t *testing.T) {
	items := []Record{{
		RecordedAt:     "2026-09-17T10:00:00Z",
		LocationID:     "loc-a",
		LocationName:   `Lab, "Alpha"`,
		DeviceID:       "dev-a",
		DeviceCode:     "SIGAP-A",
		PlacementLabel: stringPtr("North"),
		SlotID:         "slot-1",
		Stream:         "organic",
		GasRaw:         floatPtr(42),
		FisStatus:      stringPtr("Normal"),
	}}
	var output strings.Builder
	if err := WriteCSV(&output, items); err != nil {
		t.Fatalf("WriteCSV() error = %v", err)
	}
	rows, err := csv.NewReader(strings.NewReader(output.String())).ReadAll()
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 2 || rows[0][0] != "recorded_at" || rows[0][11] != "gas_raw" {
		t.Fatalf("CSV header/rows = %+v", rows)
	}
	if rows[1][2] != `Lab, "Alpha"` || rows[1][11] != "42" {
		t.Fatalf("CSV escaping/raw gas = %+v", rows[1])
	}
	if rows[1][9] != "" || rows[1][12] != "" {
		t.Fatalf("CSV null cells should be empty: %+v", rows[1])
	}
	if strings.Contains(output.String(), "credential_digest") || strings.Contains(output.String(), "fixture-secret") || strings.Contains(output.String(), "ppm") {
		t.Fatalf("CSV contains forbidden fields or labels: %s", output.String())
	}
}

func historyFixture(t *testing.T) *sql.DB {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "history.db"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO locations (id, name) VALUES ('loc-a', 'Lab Alpha'), ('loc-b', 'Lab Beta')`); err != nil {
		db.Close()
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO devices (id, location_id, device_code, placement_label, credential_digest) VALUES
		('dev-a', 'loc-a', 'SIGAP-A', 'North', 'fixture-secret'),
		('dev-b', 'loc-b', 'SIGAP-B', 'South', 'fixture-secret')`); err != nil {
		db.Close()
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO slots (id, device_id, waste_stream, operational_capacity) VALUES
		('slot-a', 'dev-a', 'organic', 10), ('slot-b', 'dev-a', 'inorganic', 10), ('slot-c', 'dev-a', 'b3', 10)`); err != nil {
		db.Close()
		t.Fatal(err)
	}
	for _, fixture := range []struct {
		slot, recorded, raw, status, safety string
		fill, weight, utilization             any
	}{
		{"slot-a", "2026-09-17T10:00:00Z", `{"slots":[{"stream":"organic","gas_raw":42}]}`, "Normal", "Clear", 50, 5, 50},
		{"slot-b", "2026-09-17T10:00:01Z", `{"slots":[{"stream":"inorganic","gas_raw":null}]}`, "Data Invalid", "Clear", 0, nil, nil},
		{"slot-c", "2026-09-17T10:00:02Z", `{"slots":[{"stream":"b3","gas_raw":99}]}`, "Normal", "Safety Alert", 10, nil, nil},
	} {
		result, err := db.Exec(`INSERT INTO readings (slot_id, recorded_at, raw_payload, fill, weight, weight_utilization) VALUES (?, ?, ?, ?, ?, ?)`, fixture.slot, fixture.recorded, fixture.raw, fixture.fill, fixture.weight, fixture.utilization)
		if err != nil {
			db.Close()
			t.Fatal(err)
		}
		readingID, err := result.LastInsertId()
		if err != nil {
			db.Close()
			t.Fatal(err)
		}
		if _, err := db.Exec(`INSERT INTO decisions (reading_id, fis_score, fis_status, fis_version, dominant_reason, safety_state) VALUES (?, ?, ?, 'fixture-v1', 'fixture reason', ?)`, readingID, map[bool]any{true: 55, false: nil}[fixture.status == "Normal"], fixture.status, fixture.safety); err != nil {
			db.Close()
			t.Fatal(err)
		}
	}
	return db
}

func floatPtr(value float64) *float64 { return &value }

func stringPtr(value string) *string { return &value }
