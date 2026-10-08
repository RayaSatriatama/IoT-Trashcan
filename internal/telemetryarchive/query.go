// Package telemetryarchive exposes bounded read-side access to the durable
// telemetry-v3 archive. Reads return persisted bytes; they never rebuild a
// batch through the current serializer.
package telemetryarchive

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
)

const (
	// DefaultBatchPageSize is used when a caller does not provide a page size.
	DefaultBatchPageSize = 50
	// MaxBatchPageSize bounds archive manifest reads independently of the
	// telemetry-v3 body and record limits.
	MaxBatchPageSize = 200
)

var (
	ErrInvalidQuery = errors.New("invalid telemetry archive query")
	ErrNotFound     = errors.New("telemetry archive item not found")
)

// BatchManifest is the durable metadata for one accepted telemetry batch.
// Body bytes are intentionally not part of this type so a list operation
// cannot unexpectedly allocate a large request body per item.
type BatchManifest struct {
	DeviceID          string
	BatchID           string
	BodyHash          string
	RecordCount       uint32
	StoredCount       uint32
	DeduplicatedCount uint32
	BodyBytes         uint32
	CreatedAt         string
	AckedAt           *string
}

// StoredBatch is a manifest together with the exact request bytes committed
// for that manifest. Body is copied before returning to keep ownership with
// the caller and prevent a driver-backed byte slice from being retained.
type StoredBatch struct {
	Manifest BatchManifest
	Body     []byte
}

// ArchiveQuery performs bounded, parameterized archive reads.
type ArchiveQuery struct {
	db *sql.DB
}

func NewArchiveQuery(db *sql.DB) (*ArchiveQuery, error) {
	if db == nil {
		return nil, fmt.Errorf("%w: database is nil", ErrInvalidQuery)
	}
	return &ArchiveQuery{db: db}, nil
}

// ListBatches lists manifests for one device in newest-first order. A limit
// of zero selects DefaultBatchPageSize; values over MaxBatchPageSize are
// clamped so a caller cannot turn a read endpoint into an unbounded scan.
func (q *ArchiveQuery) ListBatches(ctx context.Context, deviceID string, limit, offset int) ([]BatchManifest, error) {
	if q == nil || q.db == nil {
		return nil, fmt.Errorf("%w: database is unavailable", ErrInvalidQuery)
	}
	deviceID = strings.TrimSpace(deviceID)
	if deviceID == "" || offset < 0 || limit < 0 {
		return nil, fmt.Errorf("%w: device_id, limit, and offset are invalid", ErrInvalidQuery)
	}
	if limit == 0 {
		limit = DefaultBatchPageSize
	}
	if limit > MaxBatchPageSize {
		limit = MaxBatchPageSize
	}

	const statement = `
SELECT device_id, batch_id, body_hash, record_count, stored_count,
       deduplicated_count, length(body_bytes), created_at, acked_at
FROM telemetry_batches
WHERE device_id = ?
ORDER BY created_at DESC, batch_id DESC
LIMIT ? OFFSET ?`
	rows, err := q.db.QueryContext(ctx, statement, deviceID, limit, offset)
	if err != nil {
		return nil, fmt.Errorf("list telemetry batch manifests: %w", err)
	}
	defer rows.Close()

	manifests := make([]BatchManifest, 0, minInt(limit, DefaultBatchPageSize))
	for rows.Next() {
		manifest, err := scanManifest(rows)
		if err != nil {
			return nil, fmt.Errorf("scan telemetry batch manifest: %w", err)
		}
		manifests = append(manifests, manifest)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate telemetry batch manifests: %w", err)
	}
	return manifests, nil
}

// LoadBatch returns the exact body bytes persisted for a device/batch pair.
// It does not invoke telemetry serialization or canonicalization.
func (q *ArchiveQuery) LoadBatch(ctx context.Context, deviceID, batchID string) (StoredBatch, error) {
	if q == nil || q.db == nil {
		return StoredBatch{}, fmt.Errorf("%w: database is unavailable", ErrInvalidQuery)
	}
	deviceID = strings.TrimSpace(deviceID)
	batchID = strings.TrimSpace(batchID)
	if deviceID == "" || batchID == "" {
		return StoredBatch{}, fmt.Errorf("%w: device_id and batch_id are required", ErrInvalidQuery)
	}

	const statement = `
SELECT device_id, batch_id, body_hash, record_count, stored_count,
       deduplicated_count, length(body_bytes), created_at, acked_at, body_bytes
FROM telemetry_batches
WHERE device_id = ? AND batch_id = ?`
	var manifest BatchManifest
	var ackedAt sql.NullString
	var body []byte
	if err := q.db.QueryRowContext(ctx, statement, deviceID, batchID).Scan(
		&manifest.DeviceID,
		&manifest.BatchID,
		&manifest.BodyHash,
		&manifest.RecordCount,
		&manifest.StoredCount,
		&manifest.DeduplicatedCount,
		&manifest.BodyBytes,
		&manifest.CreatedAt,
		&ackedAt,
		&body,
	); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return StoredBatch{}, fmt.Errorf("%w: device=%s batch=%s", ErrNotFound, deviceID, batchID)
		}
		return StoredBatch{}, fmt.Errorf("load telemetry batch: %w", err)
	}
	if ackedAt.Valid {
		value := ackedAt.String
		manifest.AckedAt = &value
	}
	return StoredBatch{Manifest: manifest, Body: append([]byte(nil), body...)}, nil
}

// ListBatches is the function form for callers that do not need to retain a
// query object. It is intentionally a thin wrapper over ArchiveQuery.
func ListBatches(ctx context.Context, db *sql.DB, deviceID string, limit, offset int) ([]BatchManifest, error) {
	query, err := NewArchiveQuery(db)
	if err != nil {
		return nil, err
	}
	return query.ListBatches(ctx, deviceID, limit, offset)
}

// LoadBatch is the function form of ArchiveQuery.LoadBatch.
func LoadBatch(ctx context.Context, db *sql.DB, deviceID, batchID string) (StoredBatch, error) {
	query, err := NewArchiveQuery(db)
	if err != nil {
		return StoredBatch{}, err
	}
	return query.LoadBatch(ctx, deviceID, batchID)
}

type manifestScanner interface {
	Scan(dest ...any) error
}

func scanManifest(scanner manifestScanner) (BatchManifest, error) {
	var manifest BatchManifest
	var ackedAt sql.NullString
	if err := scanner.Scan(
		&manifest.DeviceID,
		&manifest.BatchID,
		&manifest.BodyHash,
		&manifest.RecordCount,
		&manifest.StoredCount,
		&manifest.DeduplicatedCount,
		&manifest.BodyBytes,
		&manifest.CreatedAt,
		&ackedAt,
	); err != nil {
		return BatchManifest{}, err
	}
	if ackedAt.Valid {
		value := ackedAt.String
		manifest.AckedAt = &value
	}
	return manifest, nil
}

func minInt(left, right int) int {
	if left < right {
		return left
	}
	return right
}
