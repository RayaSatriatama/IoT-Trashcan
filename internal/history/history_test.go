package history

import (
	"context"
	"database/sql"
	"encoding/csv"
	"net/url"
	"path/filepath"
	"strings"
	"testing"
	"time"

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
		{"status": []string{"Offline"}},
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

func TestQueryHistoryStatusUsesSafetyOverride(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	normal, err := Query(context.Background(), db, Filters{Status: "Normal"})
	if err != nil {
		t.Fatal(err)
	}
	if len(normal) != 1 || normal[0].ReadingID != 1 {
		t.Fatalf("Normal filter included overridden rows: %+v", normal)
	}

	alerts, err := Query(context.Background(), db, Filters{Status: "Safety Alert"})
	if err != nil {
		t.Fatal(err)
	}
	if len(alerts) != 1 || alerts[0].ReadingID != 3 {
		t.Fatalf("Safety Alert filter = %+v", alerts)
	}
}

func TestQueryHistoryEffectiveStatusExcludesSafetyOverrideFromUnderlyingFISFilter(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	result, err := db.Exec(`INSERT INTO readings (slot_id, recorded_at, raw_payload, fill) VALUES ('slot-c', '2026-09-17T10:00:03Z', '{"slots":[{"stream":"b3","gas_raw":99}]}', 65)`)
	if err != nil {
		t.Fatal(err)
	}
	readingID, err := result.LastInsertId()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO decisions (reading_id, fis_score, fis_status, fis_version, dominant_reason, safety_state) VALUES (?, 55, 'Waspada', 'fixture-v1', 'fixture warning', 'Safety Alert')`, readingID); err != nil {
		t.Fatal(err)
	}

	warnings, err := Query(context.Background(), db, Filters{Status: "Waspada"})
	if err != nil {
		t.Fatal(err)
	}
	if len(warnings) != 0 {
		t.Fatalf("Waspada filter included Safety Alert override: %+v", warnings)
	}

	alerts, err := Query(context.Background(), db, Filters{Status: "Safety Alert"})
	if err != nil {
		t.Fatal(err)
	}
	if len(alerts) != 2 || alerts[0].ReadingID != readingID {
		t.Fatalf("Safety Alert filter did not include effective overrides: %+v", alerts)
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
		FISStatus:      stringPtr("Normal"),
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
	if rows[1][2] != `Lab, "Alpha"` || rows[1][4] != "SIGAP-A" || rows[1][5] != "North" || rows[1][11] != "42" {
		t.Fatalf("CSV escaping/raw gas = %+v", rows[1])
	}
	if rows[1][9] != "" || rows[1][12] != "" {
		t.Fatalf("CSV null cells should be empty: %+v", rows[1])
	}
	if strings.Contains(output.String(), "credential_digest") || strings.Contains(output.String(), "fixture-secret") || strings.Contains(output.String(), "ppm") {
		t.Fatalf("CSV contains forbidden fields or labels: %s", output.String())
	}
}

func TestQueryViewSummaryIgnoresPageAndUsesEffectiveStatus(t *testing.T) {
	db := analyticsFixture(t)
	defer db.Close()

	fixedNow := time.Date(2026, time.September, 18, 12, 0, 0, 0, time.UTC)
	want := Summary{Total: 5, Normal: 1, Waspada: 1, Pickup: 1, SafetyAlert: 1, DataInvalid: 1}
	first, err := QueryView(context.Background(), db, Filters{}, PageOptions{Limit: 2}, fixedNow)
	if err != nil {
		t.Fatal(err)
	}
	second, err := QueryView(context.Background(), db, Filters{}, PageOptions{Limit: 5}, fixedNow)
	if err != nil {
		t.Fatal(err)
	}
	if first.Summary != want || second.Summary != want {
		t.Fatalf("first=%+v second=%+v", first.Summary, second.Summary)
	}
	if first.Page.TotalCount != want.Total || second.Page.TotalCount != want.Total {
		t.Fatal("page total and summary total differ")
	}
}

func TestQueryViewTrendSelectsGranularityAtBoundaries(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	cases := []struct {
		name, from, to, want string
	}{
		{"31 days", "2026-08-01T17:00:00Z", "2026-09-01T17:00:00Z", "day"},
		{"32 days", "2026-07-31T17:00:00Z", "2026-09-01T17:00:00Z", "week"},
		{"180 days", "2026-03-05T17:00:00Z", "2026-09-01T17:00:00Z", "week"},
		{"181 days", "2026-03-04T17:00:00Z", "2026-09-01T17:00:00Z", "month"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			result, err := QueryView(context.Background(), db, filterRange(t, tc.from, tc.to), PageOptions{Limit: 2}, time.Now())
			if err != nil {
				t.Fatal(err)
			}
			if result.Trend.Granularity != tc.want {
				t.Fatalf("granularity = %q, want %q", result.Trend.Granularity, tc.want)
			}
		})
	}
}

func TestQueryViewTrendDefaultsToThirtyJakartaDays(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	now := mustParseTime(t, "2026-09-18T03:45:00Z")
	result, err := QueryView(context.Background(), db, Filters{}, PageOptions{Limit: 2}, now)
	if err != nil {
		t.Fatal(err)
	}
	if !result.Trend.DefaultWindow || result.Trend.Granularity != "day" {
		t.Fatalf("trend metadata = %+v", result.Trend)
	}
	if result.Trend.ScopeFrom != "2026-08-19T17:00:00Z" || result.Trend.ScopeTo != "2026-09-18T17:00:00Z" {
		t.Fatalf("scope = %s through %s", result.Trend.ScopeFrom, result.Trend.ScopeTo)
	}
	if len(result.Trend.Buckets) != 30 || result.Trend.Buckets[0].Start != "2026-08-19T17:00:00Z" || result.Trend.Buckets[29].Start != "2026-09-17T17:00:00Z" {
		t.Fatalf("buckets = %+v", result.Trend.Buckets)
	}
}

func TestQueryViewTrendUsesMondayWeekStarts(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	result, err := QueryView(context.Background(), db, filterRange(t, "2026-07-31T17:00:00Z", "2026-09-01T17:00:00Z"), PageOptions{Limit: 2}, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if result.Trend.Granularity != "week" || len(result.Trend.Buckets) == 0 {
		t.Fatalf("trend = %+v", result.Trend)
	}
	if result.Trend.Buckets[0].Start != "2026-07-26T17:00:00Z" {
		t.Fatalf("first weekly bucket = %+v", result.Trend.Buckets[0])
	}
}

func TestQueryViewTrendUsesJakartaMidnightAndFillsMissingDays(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()
	insertHistoryDecision(t, db, "slot-a", "2026-09-01T16:59:59Z", "Normal", "Clear")
	insertHistoryDecision(t, db, "slot-b", "2026-09-01T17:00:00Z", "Waspada", "Clear")

	result, err := QueryView(context.Background(), db, filterRange(t, "2026-08-31T17:00:00Z", "2026-09-03T17:00:00Z"), PageOptions{Limit: 2}, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if len(result.Trend.Buckets) != 3 {
		t.Fatalf("bucket count = %d, want 3", len(result.Trend.Buckets))
	}
	want := []TrendBucket{
		{Start: "2026-08-31T17:00:00Z", Total: 1, Normal: 1},
		{Start: "2026-09-01T17:00:00Z", Total: 1, Waspada: 1},
		{Start: "2026-09-02T17:00:00Z"},
	}
	for index, bucket := range result.Trend.Buckets {
		if bucket.Start != want[index].Start || bucket.Total != want[index].Total || bucket.Normal != want[index].Normal || bucket.Waspada != want[index].Waspada {
			t.Fatalf("bucket[%d] = %+v, want %+v", index, bucket, want[index])
		}
	}
}

func TestQueryViewTrendUsesSafetyAlertAsEffectiveStatus(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	result, err := QueryView(context.Background(), db, filterRange(t, "2026-09-16T17:00:00Z", "2026-09-18T17:00:00Z"), PageOptions{Limit: 2}, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if len(result.Trend.Buckets) != 2 {
		t.Fatalf("bucket count = %d", len(result.Trend.Buckets))
	}
	bucket := result.Trend.Buckets[0]
	if bucket.SafetyAlert != 1 || bucket.Pickup != 0 {
		t.Fatalf("effective status counts = %+v", bucket)
	}
}

func TestQueryViewTrendCapsResponseAtLatest120Buckets(t *testing.T) {
	db := historyFixture(t)
	defer db.Close()

	result, err := QueryView(context.Background(), db, filterRange(t, "2016-08-31T17:00:00Z", "2026-10-01T17:00:00Z"), PageOptions{Limit: 2}, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if !result.Trend.Truncated || len(result.Trend.Buckets) != MaxTrendBuckets {
		t.Fatalf("trend truncation = %+v", result.Trend)
	}
	if result.Trend.Buckets[0].Start != "2016-10-31T17:00:00Z" || result.Trend.Buckets[len(result.Trend.Buckets)-1].Start != "2026-09-30T17:00:00Z" {
		t.Fatalf("capped range = first %+v last %+v", result.Trend.Buckets[0], result.Trend.Buckets[len(result.Trend.Buckets)-1])
	}
}

func filterRange(t *testing.T, from, to string) Filters {
	t.Helper()
	fromTime := mustParseTime(t, from)
	toTime := mustParseTime(t, to)
	return Filters{From: &fromTime, To: &toTime}
}

func mustParseTime(t *testing.T, value string) time.Time {
	t.Helper()
	parsed, err := time.Parse(time.RFC3339, value)
	if err != nil {
		t.Fatal(err)
	}
	return parsed
}

func analyticsFixture(t *testing.T) *sql.DB {
	t.Helper()
	db := historyFixture(t)
	insertHistoryDecision(t, db, "slot-a", "2026-09-17T10:00:03Z", "Waspada", "Clear")
	insertHistoryDecision(t, db, "slot-b", "2026-09-17T10:00:04Z", "Angkat", "Clear")
	return db
}

func insertHistoryDecision(t *testing.T, db *sql.DB, slot, recordedAt, status, safety string) {
	t.Helper()
	result, err := db.Exec(`INSERT INTO readings (slot_id, recorded_at, raw_payload, fill) VALUES (?, ?, '{"slots":[]}', 50)`, slot, recordedAt)
	if err != nil {
		t.Fatal(err)
	}
	readingID, err := result.LastInsertId()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO decisions (reading_id, fis_score, fis_status, fis_version, dominant_reason, safety_state) VALUES (?, 55, ?, 'fixture-v1', 'fixture reason', ?)`, readingID, status, safety); err != nil {
		t.Fatal(err)
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
		fill, weight, utilization           any
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
