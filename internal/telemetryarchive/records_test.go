package telemetryarchive

import (
	"bytes"
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
)

func TestListRecordsUsesUint64SortKeyCursorAndFilters(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "9", "90", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "10", "100", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "sht31_44", "organic", "boot-a", "18446744073709551615", "110", "2026-01-01T00:00:00Z", "SYNCED")

	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	first, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", Limit: 2})
	if err != nil {
		t.Fatal(err)
	}
	if len(first.Records) != 2 || first.Records[0].Sequence != "9" || first.Records[1].Sequence != "10" || first.NextCursor == nil {
		t.Fatalf("first page = %+v", first)
	}
	second, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", Cursor: *first.NextCursor})
	if err != nil {
		t.Fatal(err)
	}
	if len(second.Records) != 1 || second.Records[0].Sequence != "18446744073709551615" {
		t.Fatalf("second page = %+v", second)
	}

	filtered, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", SensorID: "sht31_44", Kind: "sample"})
	if err != nil {
		t.Fatal(err)
	}
	if len(filtered.Records) != 1 || filtered.Records[0].SensorID == nil || *filtered.Records[0].SensorID != "sht31_44" {
		t.Fatalf("sensor filter = %+v", filtered)
	}
	utcNull := true
	withoutUTC, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", UTCNull: &utcNull})
	if err != nil {
		t.Fatal(err)
	}
	if len(withoutUTC.Records) != 2 {
		t.Fatalf("utc-null filter returned %d records", len(withoutUTC.Records))
	}
}

func TestExportRecordsStreamsExactRecordEvidence(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "1", "10", "", "UNSYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if err := query.ExportRecordsJSON(context.Background(), RecordQuery{DeviceID: "device-1"}, &output); err != nil {
		t.Fatal(err)
	}
	var exported struct {
		DeviceID string `json:"device_id"`
		Records  []struct {
			Record json.RawMessage `json:"record"`
		} `json:"records"`
	}
	if err := json.Unmarshal(output.Bytes(), &exported); err != nil {
		t.Fatalf("export is not JSON: %v\n%s", err, output.String())
	}
	if exported.DeviceID != "device-1" || len(exported.Records) != 1 {
		t.Fatalf("exported envelope = %+v", exported)
	}
	var record map[string]json.RawMessage
	if err := json.Unmarshal(exported.Records[0].Record, &record); err != nil {
		t.Fatal(err)
	}
	if _, ok := record["raw"]; !ok {
		t.Fatalf("raw evidence missing from exported record: %s", exported.Records[0].Record)
	}
	if _, ok := record["profile_refs"]; !ok {
		t.Fatalf("provenance missing from exported record: %s", exported.Records[0].Record)
	}
}

func TestExportRecordsPreservesRawFormattingAndEscapes(t *testing.T) {
	db := queryTestDB(t)
	raw := []byte(`{
  "record_id": "device-1/boot-a/1",
  "payload": "<script> é \"quoted\""
}`)
	sortKey, err := telemetry.SequenceSortKey("1")
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.Exec(`INSERT INTO telemetry_records
 (device_id,record_id,record_hash,kind,boot_id,sequence_decimal,sequence_sort,monotonic_us,recorded_at,time_quality,stream,sensor_id,quality,record_bytes,first_batch_id,created_at)
 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		"device-1", "device-1/boot-a/1", "sha256:"+strings.Repeat("0", 64),
		"sample", "boot-a", "1", sortKey, "10", nil, "UNSYNCED",
		"organic", "mq4", "READY", raw, "batch", "2026-01-01T00:00:00Z")
	if err != nil {
		t.Fatal(err)
	}
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if err := query.ExportRecordsJSON(context.Background(), RecordQuery{DeviceID: "device-1"}, &output); err != nil {
		t.Fatal(err)
	}
	if !json.Valid(output.Bytes()) {
		t.Fatalf("export is not JSON: %s", output.Bytes())
	}
	needle := append([]byte(`"record":`), raw...)
	if !bytes.Contains(output.Bytes(), needle) {
		t.Fatalf("raw record bytes changed in export: want %q in %q", needle, output.Bytes())
	}
}

func TestListRecordsRejectsCursorForAnotherDevice(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "1", "10", "", "UNSYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	page, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", Limit: 1})
	if err != nil {
		t.Fatal(err)
	}
	if page.NextCursor != nil {
		t.Fatal("single item page unexpectedly produced cursor")
	}
	cursor, err := encodeRecordCursor(ArchiveRecord{DeviceID: "other", BootID: "boot-a", Sequence: "1", RecordID: "other/boot-a/1"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", Cursor: cursor}); err == nil {
		t.Fatal("cursor from another device accepted")
	}
}

func TestListRecordsCursorRemainsStableWhenNewerRecordArrives(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "1", "10", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "2", "20", "", "UNSYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	first, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", Limit: 1})
	if err != nil || first.NextCursor == nil {
		t.Fatalf("first page=%+v err=%v", first, err)
	}
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "3", "30", "", "UNSYNCED")
	second, err := query.ListRecords(context.Background(), RecordQuery{DeviceID: "device-1", Limit: 2, Cursor: *first.NextCursor})
	if err != nil {
		t.Fatal(err)
	}
	if len(second.Records) != 2 || second.Records[0].Sequence != "2" || second.Records[1].Sequence != "3" {
		t.Fatalf("cursor page after insertion=%+v", second)
	}
}
