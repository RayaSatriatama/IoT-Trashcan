// Package telemetryarchive exposes bounded read-side access to the durable
// telemetry-v3 archive. Reads return persisted bytes; they never rebuild a
// batch through the current serializer.
package telemetryarchive

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

const (
	// DefaultBatchPageSize is used when a caller does not provide a page size.
	DefaultBatchPageSize = 50
	// MaxBatchPageSize bounds archive manifest reads independently of the
	// telemetry-v3 body and record limits.
	MaxBatchPageSize = 200
	// MaxCurrentSensorSources bounds projection memory above the finite sensor
	// set supported by telemetry-v3. It is not a scan limit: every archive row
	// is still examined before this projection bound is applied.
	MaxCurrentSensorSources = 64
)

var (
	ErrInvalidQuery = errors.New("invalid telemetry archive query")
	ErrNotFound     = errors.New("telemetry archive item not found")
)

// BatchManifest is the durable metadata for one accepted telemetry batch.
// Body bytes are intentionally not part of this type so a list operation
// cannot unexpectedly allocate a large request body per item.
type BatchManifest struct {
	DeviceID          string  `json:"device_id"`
	BatchID           string  `json:"batch_id"`
	BodyHash          string  `json:"body_hash"`
	RecordCount       uint32  `json:"record_count"`
	StoredCount       uint32  `json:"stored_count"`
	DeduplicatedCount uint32  `json:"deduplicated_count"`
	BodyBytes         uint32  `json:"body_bytes"`
	CreatedAt         string  `json:"created_at"`
	AckedAt           *string `json:"acked_at"`
}

// StoredBatch is a manifest together with the exact request bytes committed
// for that manifest. Body is copied before returning to keep ownership with
// the caller and prevent a driver-backed byte slice from being retained.
type StoredBatch struct {
	Manifest BatchManifest
	Body     []byte
}

// CurrentSnapshot is a bounded read projection over the most recent archived
// sample and result for each stream. The values are the exact persisted JSON;
// this endpoint does not recalculate MCU decisions.
type CurrentSnapshot struct {
	DeviceID string `json:"device_id"`
	// Samples is indexed by stream and then sensor_id. Keeping the sensor
	// identity in the projection prevents a high-rate MQ channel from
	// replacing an independent load-cell or temperature observation.
	Samples map[string]map[string]json.RawMessage `json:"samples"`
	Results map[string]json.RawMessage            `json:"results"`
	// OrderingUnknown identifies source keys for which records from separate
	// boots did not carry a trusted comparable UTC anchor. The API deliberately
	// does not invent an order in that case.
	OrderingUnknown []string `json:"ordering_unknown,omitempty"`
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

// Current returns the newest archived sample for every sensor in each stream
// and the newest result per stream. Records are scanned from the complete
// retained archive; only the bounded projection is retained in memory.
func (q *ArchiveQuery) Current(ctx context.Context, deviceID string) (CurrentSnapshot, error) {
	if q == nil || q.db == nil || strings.TrimSpace(deviceID) == "" {
		return CurrentSnapshot{}, fmt.Errorf("%w: device_id is required", ErrInvalidQuery)
	}
	snapshot := CurrentSnapshot{DeviceID: deviceID, Samples: map[string]map[string]json.RawMessage{}, Results: map[string]json.RawMessage{}}
	rows, err := q.db.QueryContext(ctx, `
	SELECT kind, record_bytes
FROM telemetry_records
WHERE device_id = ? AND kind IN ('sample', 'result')
ORDER BY rowid ASC`, deviceID)
	if err != nil {
		return CurrentSnapshot{}, fmt.Errorf("query current telemetry: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var kind string
		var raw []byte
		if err := rows.Scan(&kind, &raw); err != nil {
			return CurrentSnapshot{}, fmt.Errorf("scan current telemetry: %w", err)
		}
		evidence, err := parseCurrentEvidence(raw, kind)
		if err != nil || evidence.stream == "" {
			continue
		}
		key := evidence.stream
		if kind == "sample" {
			if evidence.sensorID == "" {
				continue
			}
			if _, ok := snapshot.Samples[key]; !ok {
				if currentSensorCount(snapshot.Samples) >= MaxCurrentSensorSources {
					continue
				}
				snapshot.Samples[key] = map[string]json.RawMessage{}
			}
			previousRaw, exists := snapshot.Samples[key][evidence.sensorID]
			if !exists {
				snapshot.Samples[key][evidence.sensorID] = append(json.RawMessage(nil), raw...)
				continue
			}
			previous, parseErr := parseCurrentEvidence(previousRaw, kind)
			if parseErr != nil {
				continue
			}
			comparison := compareCurrentEvidence(previous, evidence)
			switch comparison {
			case currentNewer:
				snapshot.Samples[key][evidence.sensorID] = append(json.RawMessage(nil), raw...)
			case currentUnknown:
				snapshot.OrderingUnknown = appendUnique(snapshot.OrderingUnknown, key+"/"+evidence.sensorID)
			}
			continue
		}
		target := snapshot.Results
		previousRaw, exists := target[key]
		if !exists {
			target[key] = append(json.RawMessage(nil), raw...)
			continue
		}
		previous, parseErr := parseCurrentEvidence(previousRaw, kind)
		if parseErr != nil {
			continue
		}
		switch compareCurrentEvidence(previous, evidence) {
		case currentNewer:
			target[key] = append(json.RawMessage(nil), raw...)
		case currentUnknown:
			snapshot.OrderingUnknown = appendUnique(snapshot.OrderingUnknown, key)
		}
	}
	if err := rows.Err(); err != nil {
		return CurrentSnapshot{}, fmt.Errorf("iterate current telemetry: %w", err)
	}
	return snapshot, nil
}

func Current(ctx context.Context, db *sql.DB, deviceID string) (CurrentSnapshot, error) {
	query, err := NewArchiveQuery(db)
	if err != nil {
		return CurrentSnapshot{}, err
	}
	return query.Current(ctx, deviceID)
}

type currentComparison uint8

const (
	currentNotNewer currentComparison = iota
	currentNewer
	currentUnknown
)

type currentEvidence struct {
	stream      string
	sensorID    string
	bootID      string
	sequence    uint64
	monotonic   uint64
	recordedAt  time.Time
	hasUTC      bool
	timeQuality string
}

// parseCurrentEvidence reads only the immutable common fields needed for
// ordering. The raw record remains the response payload, so no evidence or
// provenance is reconstructed by the projection.
func parseCurrentEvidence(raw []byte, kind string) (currentEvidence, error) {
	var fields struct {
		Stream      string  `json:"stream"`
		SensorID    string  `json:"sensor_id"`
		BootID      string  `json:"boot_id"`
		Sequence    string  `json:"sequence"`
		Monotonic   string  `json:"monotonic_us"`
		RecordedAt  *string `json:"recorded_at"`
		TimeQuality string  `json:"time_quality"`
	}
	if err := json.Unmarshal(raw, &fields); err != nil {
		return currentEvidence{}, err
	}
	sequence, err := strconv.ParseUint(fields.Sequence, 10, 64)
	if err != nil || fields.BootID == "" || fields.Stream == "" {
		return currentEvidence{}, errors.New("invalid current record identity")
	}
	monotonic, err := strconv.ParseUint(fields.Monotonic, 10, 64)
	if err != nil {
		return currentEvidence{}, errors.New("invalid current monotonic timestamp")
	}
	evidence := currentEvidence{
		stream: fields.Stream, sensorID: fields.SensorID, bootID: fields.BootID,
		sequence: sequence, monotonic: monotonic,
		timeQuality: fields.TimeQuality,
	}
	if fields.RecordedAt != nil && *fields.RecordedAt != "" && fields.TimeQuality != "UNSYNCED" {
		if value, parseErr := time.Parse(time.RFC3339Nano, *fields.RecordedAt); parseErr == nil {
			evidence.recordedAt = value
			evidence.hasUTC = true
		}
	}
	if kind == "result" {
		evidence.sensorID = ""
	}
	return evidence, nil
}

func currentSensorCount(samples map[string]map[string]json.RawMessage) int {
	count := 0
	for _, sensors := range samples {
		count += len(sensors)
	}
	return count
}

func compareCurrentEvidence(previous, candidate currentEvidence) currentComparison {
	if previous.bootID == candidate.bootID {
		if candidate.sequence > previous.sequence ||
			(candidate.sequence == previous.sequence && candidate.monotonic > previous.monotonic) {
			return currentNewer
		}
		return currentNotNewer
	}
	// Monotonic clocks have no ordering relationship across boots. A trusted
	// UTC anchor is the only cross-boot ordering evidence accepted here.
	if previous.hasUTC && candidate.hasUTC {
		if candidate.recordedAt.After(previous.recordedAt) {
			return currentNewer
		}
		return currentNotNewer
	}
	if candidate.hasUTC != previous.hasUTC {
		return currentUnknown
	}
	return currentUnknown
}

func appendUnique(values []string, value string) []string {
	for _, existing := range values {
		if existing == value {
			return values
		}
	}
	return append(values, value)
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
