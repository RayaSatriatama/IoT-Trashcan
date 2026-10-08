package telemetryarchive

import (
	"context"
	"database/sql"
	"errors"
	"reflect"
	"testing"

	_ "modernc.org/sqlite"
)

func queryTestDB(t *testing.T) *sql.DB {
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
`)
	if err != nil {
		t.Fatal(err)
	}
	return db
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
