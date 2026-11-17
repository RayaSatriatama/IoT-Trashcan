package telemetryarchive

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"
)

func TestHistorySensorAllowListsRejectForeignAndDerivedFields(t *testing.T) {
	checks := []struct {
		sensor, field string
	}{
		{"hx711_organic", "adc_counts"}, {"mq4", "counts"},
		{"ultrasonic_b3", "adc_counts"}, {"ds18b20_b3", "rom"},
		{"sht31_44", "temperature_raw"}, {"ultrasonic_organic", "fill_percent"},
	}
	for _, check := range checks {
		for _, group := range historySensors[check.sensor] {
			if group[check.field] {
				t.Errorf("%s unexpectedly allows %s", check.sensor, check.field)
			}
		}
	}
}

func TestSensorHistoryPagesNewestMonotonicWithScopedCursor(t *testing.T) {
	db := queryTestDB(t)
	q, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "1", "2", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "2", "10", "", "UNSYNCED")
	first, err := q.ListSensorHistory(context.Background(), "device-1", "mq4", "boot-a", "", 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(first.Points) != 1 || first.Points[0].MonotonicUS != "10" || first.NextCursor == nil {
		t.Fatalf("unexpected first page: %#v", first)
	}
	second, err := q.ListSensorHistory(context.Background(), "device-1", "mq4", "boot-a", *first.NextCursor, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(second.Points) != 1 || second.Points[0].MonotonicUS != "2" || second.NextCursor != nil {
		t.Fatalf("unexpected second page: %#v", second)
	}
	if _, err = q.ListSensorHistory(context.Background(), "device-1", "mq135", "boot-a", *first.NextCursor, 1); err == nil {
		t.Fatal("sensor cursor context accepted")
	}
	if _, err = q.ListSensorHistory(context.Background(), "device-1", "mq4", "boot-b", *first.NextCursor, 1); err == nil {
		t.Fatal("boot cursor context accepted")
	}
	if _, err = q.ListSensorHistory(context.Background(), "device-2", "mq4", "boot-a", *first.NextCursor, 1); err == nil {
		t.Fatal("device cursor context accepted")
	}
}

func TestSensorHistorySanitizationAndUnavailableBoot(t *testing.T) {
	db := queryTestDB(t)
	q, _ := NewArchiveQuery(db)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "1", "1", "", "UNSYNCED")
	_, err := db.Exec(`UPDATE telemetry_records SET record_bytes=? WHERE device_id='device-1' AND record_id='device-1/boot-a/1'`, []byte(`{"device_id":"device-1","record_id":"device-1/boot-a/1","sensor_id":"mq4","boot_id":"boot-a","sequence":"1","monotonic_us":"1","quality":"READY","reason":"DATA_READY","raw":{"adc_counts":0,"counts":9,"fill_percent":88,"private":"secret"},"converted":{"adc_mv":0,"kg":42}}`))
	if err != nil {
		t.Fatal(err)
	}
	page, err := q.ListSensorHistory(context.Background(), "device-1", "mq4", "boot-a", "", 0)
	if err != nil {
		t.Fatal(err)
	}
	p := page.Points[0]
	if p.Raw["adc_counts"] != 0 || len(p.Raw) != 1 || p.Converted["adc_mv"] != 0 || len(p.Converted) != 1 || p.RecordedAt != nil || p.TimeQuality != "UNSYNCED" || p.SampleOrigin != "UNKNOWN" {
		t.Fatalf("sanitization/time mismatch: %#v", p)
	}
	empty, err := q.ListSensorHistory(context.Background(), "device-1", "mq4", "", "", 0)
	if err != nil || len(empty.Points) != 0 || empty.UnavailableReason == nil {
		t.Fatalf("missing boot not explicit: %#v %v", empty, err)
	}
}

func TestSensorHistoryRejectsInvalidIdentifiersAndCursor(t *testing.T) {
	db := queryTestDB(t)
	q, _ := NewArchiveQuery(db)
	for _, tc := range []struct {
		device, sensor, boot, cursor string
		limit                        int
	}{{" device-1", "mq4", "boot-a", "", 1}, {"device-1", "mq4 ", "boot-a", "", 1}, {"device-1", "mq4", "boot-a", "not-base64!", 1}, {"device-1", "mq4", "boot-a", "", -1}, {"device-1", "mq4", "bad/boot", "", 1}} {
		if _, err := q.ListSensorHistory(context.Background(), tc.device, tc.sensor, tc.boot, tc.cursor, tc.limit); err == nil {
			t.Fatalf("accepted invalid query %#v", tc)
		}
	}
}

func TestHistoryCursorRejectsMalformedNumericFieldsWithoutPanic(t *testing.T) {
	db := queryTestDB(t)
	q, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	base := historyCursor{Version: 1, Device: "device-1", Sensor: "mq4", Boot: "boot-a", BoundaryMono: "1", BoundarySeq: "1", BoundaryID: "a", Mono: "1", Sequence: "1", RecordID: "a", Snapshot: 1}
	for _, field := range []string{"BoundaryMono", "BoundarySeq", "Mono", "Sequence"} {
		for _, bad := range []string{"", "01", "-1", "18446744073709551616", strings.Repeat("9", 21)} {
			c := base
			switch field {
			case "BoundaryMono":
				c.BoundaryMono = bad
			case "BoundarySeq":
				c.BoundarySeq = bad
			case "Mono":
				c.Mono = bad
			default:
				c.Sequence = bad
			}
			raw, _ := json.Marshal(c)
			token := base64.RawURLEncoding.EncodeToString(raw)
			if _, err := q.ListSensorHistory(context.Background(), "device-1", "mq4", "boot-a", token, 1); err == nil {
				t.Errorf("accepted %s=%q", field, bad)
			}
		}
	}
}

func TestHistoryAllowsDS18B20TemperatureRaw(t *testing.T) {
	got := historyNumbers(map[string]json.RawMessage{"temperature_raw": json.RawMessage(`23.5`), "temperature_c": json.RawMessage(`22`)}, historySensors["ds18b20_b3"]["counts"])
	if got["temperature_raw"] != 23.5 || len(got) != 1 {
		t.Fatalf("unexpected DS18B20 raw projection: %#v", got)
	}
}
