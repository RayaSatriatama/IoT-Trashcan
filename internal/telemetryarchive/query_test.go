package telemetryarchive

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
	_ "modernc.org/sqlite"
)

func queryTestDB(t testing.TB) *sql.DB {
	t.Helper()
	db, err := sql.Open("sqlite", "file:telemetry-archive-query?mode=memory&cache=private")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	_, err = db.Exec(`
CREATE TABLE telemetry_batches (
  device_id TEXT NOT NULL,
  batch_id TEXT NOT NULL,
  body_hash TEXT NOT NULL,
  body_bytes BLOB NOT NULL,
  record_count INTEGER NOT NULL,
  stored_count INTEGER NOT NULL,
  deduplicated_count INTEGER NOT NULL,
  created_at TEXT NOT NULL,
  acked_at TEXT,
  PRIMARY KEY (device_id, batch_id)
);
CREATE TABLE telemetry_records (
  device_id TEXT NOT NULL,
  record_id TEXT NOT NULL,
  record_hash TEXT NOT NULL,
  kind TEXT NOT NULL,
  boot_id TEXT NOT NULL,
  sequence_decimal TEXT NOT NULL,
  sequence_sort TEXT NOT NULL,
  monotonic_us TEXT NOT NULL,
  recorded_at TEXT,
  time_quality TEXT NOT NULL,
  stream TEXT,
  sensor_id TEXT,
  quality TEXT,
  record_bytes BLOB NOT NULL,
  first_batch_id TEXT NOT NULL,
  created_at TEXT NOT NULL,
  PRIMARY KEY (device_id, record_id)
);
CREATE TABLE telemetry_current_candidates (
  device_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  stream TEXT NOT NULL,
  sensor_id TEXT NOT NULL DEFAULT '',
  boot_id TEXT NOT NULL,
  record_id TEXT NOT NULL,
  record_hash TEXT NOT NULL,
  sequence_sort TEXT NOT NULL,
  monotonic_us TEXT NOT NULL,
  recorded_at TEXT,
  time_quality TEXT NOT NULL,
  record_bytes BLOB NOT NULL,
  first_record_rowid INTEGER NOT NULL,
  PRIMARY KEY (device_id, kind, stream, sensor_id, boot_id)
);
CREATE TRIGGER telemetry_records_current_candidate
AFTER INSERT ON telemetry_records
WHEN NEW.kind IN ('sample', 'result') AND NEW.stream IS NOT NULL
BEGIN
  INSERT INTO telemetry_current_candidates
    (device_id,kind,stream,sensor_id,boot_id,record_id,record_hash,
     sequence_sort,monotonic_us,recorded_at,time_quality,record_bytes,
     first_record_rowid)
  VALUES (NEW.device_id,NEW.kind,NEW.stream,COALESCE(NEW.sensor_id,''),
    NEW.boot_id,NEW.record_id,NEW.record_hash,NEW.sequence_sort,
    NEW.monotonic_us,NEW.recorded_at,NEW.time_quality,NEW.record_bytes,NEW.rowid)
  ON CONFLICT(device_id,kind,stream,sensor_id,boot_id) DO UPDATE SET
    record_id=excluded.record_id, record_hash=excluded.record_hash,
    sequence_sort=excluded.sequence_sort, monotonic_us=excluded.monotonic_us,
    recorded_at=excluded.recorded_at, time_quality=excluded.time_quality,
    record_bytes=excluded.record_bytes
  WHERE excluded.sequence_sort > telemetry_current_candidates.sequence_sort
     OR (excluded.sequence_sort = telemetry_current_candidates.sequence_sort
         AND (length(excluded.monotonic_us) > length(telemetry_current_candidates.monotonic_us)
              OR (length(excluded.monotonic_us) = length(telemetry_current_candidates.monotonic_us)
                  AND excluded.monotonic_us > telemetry_current_candidates.monotonic_us)))
     OR (excluded.sequence_sort = telemetry_current_candidates.sequence_sort
         AND excluded.monotonic_us = telemetry_current_candidates.monotonic_us
         AND excluded.record_id > telemetry_current_candidates.record_id);
END;
`)
	if err != nil {
		t.Fatal(err)
	}
	return db
}

func insertQueryRecord(t *testing.T, db *sql.DB, kind, sensor, stream, boot, sequence, monotonic, recordedAt, quality string) {
	t.Helper()
	object := map[string]any{
		"record_id":   "device-1/" + boot + "/" + sequence,
		"record_hash": "sha256:" + strings.Repeat("0", 64),
		"kind":        kind, "device_id": "device-1", "boot_id": boot,
		"sequence": sequence, "monotonic_us": monotonic, "recorded_at": nil,
		"time_quality": quality, "config_ref": map[string]any{"schema_version": 1, "config_version": "1", "config_hash": "sha256:" + strings.Repeat("0", 64)}, "profile_refs": []any{},
		"sensor_id": sensor, "stream": stream, "quality": "READY", "reason": "DATA_READY",
		"raw": map[string]any{"adc_counts": 1}, "converted": nil,
	}
	if recordedAt != "" {
		object["recorded_at"] = recordedAt
	}
	if kind == "result" {
		delete(object, "sensor_id")
		delete(object, "quality")
		delete(object, "reason")
		delete(object, "raw")
		object["computed_monotonic_us"] = monotonic
		object["source_sample_ids"] = []string{"device-1/" + boot + "/1"}
		object["processed"] = map[string]any{"accumulation_hours": nil, "fill_pct": nil, "ogi": nil, "temperature_c": nil, "weight_kg": nil, "weight_utilization_pct": nil}
		object["fuzzy"] = map[string]any{"state": "UNAVAILABLE", "score": nil, "label": nil, "reason": "INPUT_MISSING", "model_state": "UNCONFIGURED", "trace": nil}
		object["safety"] = map[string]any{"state": "UNAVAILABLE", "reason": "INPUT_MISSING", "model_state": "UNCONFIGURED", "trace": nil}
	}
	encoded, err := json.Marshal(object)
	if err != nil {
		t.Fatal(err)
	}
	hash, _, err := telemetry.RecordHash(encoded)
	if err != nil {
		t.Fatal(err)
	}
	object["record_hash"] = hash
	encoded, err = json.Marshal(object)
	if err != nil {
		t.Fatal(err)
	}
	sortKey, err := telemetry.SequenceSortKey(sequence)
	if err != nil {
		t.Fatal(err)
	}
	var recorded any
	if recordedAt != "" {
		recorded = recordedAt
	}
	var sensorValue, qualityValue any
	if kind == "sample" {
		sensorValue = sensor
		qualityValue = "READY"
	}
	_, err = db.Exec(`INSERT INTO telemetry_records
 (device_id,record_id,record_hash,kind,boot_id,sequence_decimal,sequence_sort,monotonic_us,recorded_at,time_quality,stream,sensor_id,quality,record_bytes,first_batch_id,created_at)
 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`, "device-1", object["record_id"], hash, kind, boot, sequence, sortKey, monotonic, recorded, quality, stream, sensorValue, qualityValue, encoded, "batch", "2026-01-01T00:00:00Z")
	if err != nil {
		t.Fatal(err)
	}
}

func insertQueryBatch(t *testing.T, db *sql.DB, device, batch, created string, body []byte) {
	t.Helper()
	_, err := db.Exec(`INSERT INTO telemetry_batches
 (device_id,batch_id,body_hash,body_bytes,record_count,stored_count,deduplicated_count,created_at,acked_at)
 VALUES (?,?,?,?,?,?,?,?,?)`, device, batch, "sha256:test-"+batch, body, 2, 2, 0, created, nil)
	if err != nil {
		t.Fatal(err)
	}
}

func TestListBatchesIsBoundedAndNewestFirst(t *testing.T) {
	db := queryTestDB(t)
	insertQueryBatch(t, db, "device-1", "batch-a", "2026-01-01T00:00:00Z", []byte("a"))
	insertQueryBatch(t, db, "device-1", "batch-b", "2026-01-02T00:00:00Z", []byte("b"))
	insertQueryBatch(t, db, "device-1", "batch-c", "2026-01-03T00:00:00Z", []byte("c"))
	insertQueryBatch(t, db, "device-2", "batch-other", "2026-01-04T00:00:00Z", []byte("x"))

	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	items, err := query.ListBatches(context.Background(), "device-1", MaxBatchPageSize+100, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 3 || items[0].BatchID != "batch-c" || items[2].BatchID != "batch-a" {
		t.Fatalf("unexpected manifests: %+v", items)
	}
	page, err := query.ListBatches(context.Background(), "device-1", 1, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(page) != 1 || page[0].BatchID != "batch-b" {
		t.Fatalf("bounded page = %+v, want batch-b", page)
	}
}

func TestLoadBatchReplaysExactStoredBytes(t *testing.T) {
	db := queryTestDB(t)
	body := []byte(`{"batch_id":"b","number":1.0}`)
	insertQueryBatch(t, db, "device-1", "batch-a", "2026-01-01T00:00:00Z", body)
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	loaded, err := query.LoadBatch(context.Background(), "device-1", "batch-a")
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(loaded.Body, body) {
		t.Fatalf("body changed during replay: got %q want %q", loaded.Body, body)
	}
	loaded.Body[0] = 'X'
	again, err := query.LoadBatch(context.Background(), "device-1", "batch-a")
	if err != nil {
		t.Fatal(err)
	}
	if again.Body[0] != '{' {
		t.Fatal("returned body aliases persisted query buffer")
	}
}

func TestLoadBatchMissingDeviceOrBatch(t *testing.T) {
	db := queryTestDB(t)
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	_, err = query.LoadBatch(context.Background(), "missing", "batch")
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("missing batch error = %v, want ErrNotFound", err)
	}
	if _, err := query.ListBatches(context.Background(), "", 1, 0); !errors.Is(err, ErrInvalidQuery) {
		t.Fatalf("empty device error = %v, want ErrInvalidQuery", err)
	}
	if _, err := query.ListBatches(context.Background(), "device", -1, 0); !errors.Is(err, ErrInvalidQuery) {
		t.Fatalf("negative limit error = %v, want ErrInvalidQuery", err)
	}
}

func TestCurrentKeepsIndependentSensorsAndRejectsLateSameBootBacklog(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "1", "100", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "hx711_organic", "organic", "boot-a", "2", "200", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "3", "300", "", "UNSYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	if len(snapshot.Samples["organic"]) != 2 {
		t.Fatalf("samples=%v, want two sensors", snapshot.Samples)
	}
	var mq map[string]any
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &mq); err != nil {
		t.Fatal(err)
	}
	if mq["sequence"] != "3" {
		t.Fatalf("late same-boot record replaced current: %#v", mq)
	}
}

func TestCurrentDoesNotInventCrossBootOrderWithoutUTC(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "9", "900", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-b", "1", "1", "", "UNSYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	var current map[string]any
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &current); err != nil {
		t.Fatal(err)
	}
	if current["boot_id"] != "boot-a" {
		t.Fatalf("unknown cross-boot order changed current: %#v", current)
	}
	if len(snapshot.OrderingUnknown) != 1 || snapshot.OrderingUnknown[0] != "organic/mq4" {
		t.Fatalf("unknown ordering=%v", snapshot.OrderingUnknown)
	}
}

func TestCurrentUsesTrustedUTCAcrossBoots(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "9", "900", "2026-01-01T00:00:00Z", "SYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-b", "1", "1", "2026-01-01T00:00:01Z", "SYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	var current map[string]any
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &current); err != nil {
		t.Fatal(err)
	}
	if current["boot_id"] != "boot-b" {
		t.Fatalf("trusted UTC did not advance current: %#v", current)
	}
}

func TestCurrentDoesNotCompareTrustedUTCWithNullUTCAcrossBoots(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "9", "900", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-b", "1", "1", "2026-01-01T00:00:01Z", "SYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	var current map[string]any
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &current); err != nil {
		t.Fatal(err)
	}
	if current["boot_id"] != "boot-a" {
		t.Fatalf("cross-boot UTC/null comparison invented ordering: %#v", current)
	}
	if len(snapshot.OrderingUnknown) != 1 || snapshot.OrderingUnknown[0] != "organic/mq4" {
		t.Fatalf("unknown ordering=%v", snapshot.OrderingUnknown)
	}
}

func TestCurrentDoesNotOrderUTCAgainstUTCNullAcrossBoots(t *testing.T) {
	db := queryTestDB(t)
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", "9", "900", "", "UNSYNCED")
	insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-b", "1", "1", "2026-01-01T00:00:01Z", "SYNCED")

	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	var current struct {
		BootID string `json:"boot_id"`
	}
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &current); err != nil {
		t.Fatal(err)
	}
	if current.BootID != "boot-a" {
		t.Fatalf("UTC versus UTC-null record changed current: %#v", current)
	}
	if len(snapshot.OrderingUnknown) != 1 || snapshot.OrderingUnknown[0] != "organic/mq4" {
		t.Fatalf("ordering uncertainty = %v", snapshot.OrderingUnknown)
	}
}

func TestCurrentScansBeyondTwoHundredRowsForSparseSensor(t *testing.T) {
	db := queryTestDB(t)
	for i := 1; i <= 201; i++ {
		insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", fmt.Sprint(i), fmt.Sprint(i), "", "UNSYNCED")
	}
	insertQueryRecord(t, db, "sample", "sht31_44", "organic", "boot-a", "202", "202", "", "UNSYNCED")
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	if len(snapshot.Samples["organic"]) != 2 {
		t.Fatalf("sparse source was lost: %v", snapshot.Samples["organic"])
	}
}

func TestCurrentProjectionRetainsOnlyPerBootCandidates(t *testing.T) {
	db := queryTestDB(t)
	for i := 1; i <= 5000; i++ {
		insertQueryRecord(t, db, "sample", "mq4", "organic", "boot-a", fmt.Sprint(i), fmt.Sprint(i), "", "UNSYNCED")
	}
	var candidates int
	if err := db.QueryRow(`SELECT COUNT(*) FROM telemetry_current_candidates`).Scan(&candidates); err != nil {
		t.Fatal(err)
	}
	if candidates != 1 {
		t.Fatalf("candidate projection retained %d rows for one source/boot, want 1", candidates)
	}
	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	var current map[string]any
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &current); err != nil {
		t.Fatal(err)
	}
	if current["sequence"] != "5000" {
		t.Fatalf("projection current=%v, want sequence 5000", current["sequence"])
	}
}

func TestCurrentProjectionComparesMonotonicAsDecimalString(t *testing.T) {
	db := queryTestDB(t)
	sortKey, err := telemetry.SequenceSortKey("10")
	if err != nil {
		t.Fatal(err)
	}
	insert := func(recordID, monotonic string) {
		t.Helper()
		raw := []byte(fmt.Sprintf(`{"stream":"organic","sensor_id":"mq4","boot_id":"boot-a","sequence":"10","monotonic_us":"%s","recorded_at":null,"time_quality":"UNSYNCED"}`, monotonic))
		if _, err := db.Exec(`INSERT INTO telemetry_records
 (device_id,record_id,record_hash,kind,boot_id,sequence_decimal,sequence_sort,monotonic_us,recorded_at,time_quality,stream,sensor_id,quality,record_bytes,first_batch_id,created_at)
 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`, "device-1", recordID,
			"sha256:"+strings.Repeat("0", 64), "sample", "boot-a", "10",
			sortKey, monotonic, nil, "UNSYNCED", "organic", "mq4",
			"READY", raw, "batch", "2026-01-01T00:00:00Z"); err != nil {
			t.Fatal(err)
		}
	}
	insert("device-1/boot-a/10-a", "900")
	insert("device-1/boot-a/10-b", "1000")

	query, err := NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := query.Current(context.Background(), "device-1")
	if err != nil {
		t.Fatal(err)
	}
	var current map[string]any
	if err := json.Unmarshal(snapshot.Samples["organic"]["mq4"], &current); err != nil {
		t.Fatal(err)
	}
	if current["monotonic_us"] != "1000" {
		t.Fatalf("projection compared monotonic lexically: %#v", current)
	}
}

func seedProjectionBenchmarkRows(tb testing.TB, db *sql.DB, count int) {
	tb.Helper()
	tx, err := db.Begin()
	if err != nil {
		tb.Fatal(err)
	}
	for i := 1; i <= count; i++ {
		sequence := fmt.Sprint(i)
		sortKey, err := telemetry.SequenceSortKey(sequence)
		if err != nil {
			tx.Rollback()
			tb.Fatal(err)
		}
		raw := []byte(fmt.Sprintf(`{"stream":"organic","sensor_id":"mq4","boot_id":"boot-a","sequence":"%s","monotonic_us":"%s","recorded_at":null,"time_quality":"UNSYNCED"}`, sequence, sequence))
		if _, err := tx.Exec(`INSERT INTO telemetry_records
 (device_id,record_id,record_hash,kind,boot_id,sequence_decimal,sequence_sort,monotonic_us,recorded_at,time_quality,stream,sensor_id,quality,record_bytes,first_batch_id,created_at)
 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`, "device-1", "device-1/boot-a/"+sequence,
			"sha256:"+strings.Repeat("0", 64), "sample", "boot-a", sequence, sortKey,
			sequence, nil, "UNSYNCED", "organic", "mq4", "READY", raw, "batch", "2026-01-01T00:00:00Z"); err != nil {
			tx.Rollback()
			tb.Fatal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		tb.Fatal(err)
	}
}

func BenchmarkCurrentProjectionScalesWithCandidateCount(b *testing.B) {
	for _, count := range []int{100, 1000, 10000} {
		b.Run(fmt.Sprintf("records_%d", count), func(b *testing.B) {
			db := queryTestDB(b)
			seedProjectionBenchmarkRows(b, db, count)
			query, err := NewArchiveQuery(db)
			if err != nil {
				b.Fatal(err)
			}
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if _, err := query.Current(context.Background(), "device-1"); err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}
