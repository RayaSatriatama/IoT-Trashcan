package telemetryarchive

import (
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"strings"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
)

const (
	DefaultRecordPageSize = 50
	MaxRecordPageSize     = 200
)

// RecordQuery is the bounded record-level archive query contract. Sequence
// ordering uses the persisted decimal string and 20-byte sort key so SQLite
// never has to coerce uint64 values into signed INTEGER.
type RecordQuery struct {
	DeviceID string
	Limit    int
	Cursor   string
	Kind     string
	Stream   string
	SensorID string
	Quality  string
	UTCNull  *bool
}

type ArchiveRecord struct {
	DeviceID    string          `json:"device_id"`
	RecordID    string          `json:"record_id"`
	RecordHash  string          `json:"record_hash"`
	Kind        string          `json:"kind"`
	BootID      string          `json:"boot_id"`
	Sequence    string          `json:"sequence"`
	MonotonicUS string          `json:"monotonic_us"`
	RecordedAt  *string         `json:"recorded_at"`
	TimeQuality string          `json:"time_quality"`
	Stream      *string         `json:"stream,omitempty"`
	SensorID    *string         `json:"sensor_id,omitempty"`
	Quality     *string         `json:"quality,omitempty"`
	Record      json.RawMessage `json:"record"`
}

type RecordPage struct {
	DeviceID   string          `json:"device_id"`
	Records    []ArchiveRecord `json:"records"`
	NextCursor *string         `json:"next_cursor,omitempty"`
}

// WriteRecordPageJSON writes a record page without passing persisted record
// bytes through encoding/json. The HTTP page endpoint uses this writer so its
// nested record evidence has the same byte-preservation guarantee as export.
func WriteRecordPageJSON(writer io.Writer, page RecordPage) error {
	if writer == nil {
		return fmt.Errorf("invalid record page writer")
	}
	device, err := json.Marshal(page.DeviceID)
	if err != nil {
		return err
	}
	if _, err := fmt.Fprintf(writer, `{"device_id":%s,"records":[`, device); err != nil {
		return err
	}
	for index, record := range page.Records {
		if index > 0 {
			if _, err := io.WriteString(writer, ","); err != nil {
				return err
			}
		}
		if err := writeArchiveRecordJSON(writer, record); err != nil {
			return err
		}
	}
	if _, err := io.WriteString(writer, "]"); err != nil {
		return err
	}
	if page.NextCursor != nil {
		cursor, err := json.Marshal(*page.NextCursor)
		if err != nil {
			return err
		}
		if _, err := fmt.Fprintf(writer, `,"next_cursor":%s`, cursor); err != nil {
			return err
		}
	}
	_, err = io.WriteString(writer, "}")
	return err
}

// ValidateRecordQuery checks the bounded, cursor-bearing portion of a record
// read before a streaming response commits its HTTP headers. Export callers
// use this preflight so malformed cursors cannot produce a misleading 200
// status followed by an empty/truncated body.
func ValidateRecordQuery(request RecordQuery) error {
	request.DeviceID = strings.TrimSpace(request.DeviceID)
	if request.DeviceID == "" || request.Limit < 0 {
		return fmt.Errorf("%w: device_id and limit are required", ErrInvalidQuery)
	}
	cursor, err := decodeRecordCursor(request.Cursor)
	if err != nil {
		return err
	}
	if cursor.DeviceID != "" && cursor.DeviceID != request.DeviceID {
		return fmt.Errorf("%w: cursor device does not match query", ErrInvalidQuery)
	}
	return nil
}

type recordCursor struct {
	DeviceID string `json:"device_id"`
	BootID   string `json:"boot_id"`
	Sequence string `json:"sequence"`
	RecordID string `json:"record_id"`
}

func encodeRecordCursor(record ArchiveRecord) (string, error) {
	raw, err := json.Marshal(recordCursor{
		DeviceID: record.DeviceID,
		BootID:   record.BootID,
		Sequence: record.Sequence,
		RecordID: record.RecordID,
	})
	if err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(raw), nil
}

func decodeRecordCursor(value string) (recordCursor, error) {
	if strings.TrimSpace(value) == "" {
		return recordCursor{}, nil
	}
	raw, err := base64.RawURLEncoding.DecodeString(value)
	if err != nil {
		return recordCursor{}, fmt.Errorf("%w: cursor is malformed", ErrInvalidQuery)
	}
	var cursor recordCursor
	if err := json.Unmarshal(raw, &cursor); err != nil {
		return recordCursor{}, fmt.Errorf("%w: cursor is malformed", ErrInvalidQuery)
	}
	if cursor.DeviceID == "" || cursor.BootID == "" || cursor.Sequence == "" || cursor.RecordID == "" {
		return recordCursor{}, fmt.Errorf("%w: cursor is incomplete", ErrInvalidQuery)
	}
	if _, err := sequenceSortKey(cursor.Sequence); err != nil {
		return recordCursor{}, err
	}
	return cursor, nil
}

// ListRecords returns a stable cursor page of exact record evidence. The page
// is bounded, but the archive scan is not restricted to a recent-record guess.
func (q *ArchiveQuery) ListRecords(ctx context.Context, request RecordQuery) (RecordPage, error) {
	if q == nil || q.db == nil {
		return RecordPage{}, fmt.Errorf("%w: database is unavailable", ErrInvalidQuery)
	}
	if err := ValidateRecordQuery(request); err != nil {
		return RecordPage{}, err
	}
	request.DeviceID = strings.TrimSpace(request.DeviceID)
	if request.Limit == 0 {
		request.Limit = DefaultRecordPageSize
	}
	if request.Limit > MaxRecordPageSize {
		request.Limit = MaxRecordPageSize
	}
	cursor, _ := decodeRecordCursor(request.Cursor)

	statement, args, err := recordQuerySQL(request, cursor, request.Limit+1)
	if err != nil {
		return RecordPage{}, err
	}
	rows, err := q.db.QueryContext(ctx, statement, args...)
	if err != nil {
		return RecordPage{}, fmt.Errorf("list telemetry records: %w", err)
	}
	defer rows.Close()

	page := RecordPage{DeviceID: request.DeviceID, Records: make([]ArchiveRecord, 0, request.Limit)}
	for rows.Next() {
		record, err := scanArchiveRecord(rows)
		if err != nil {
			return RecordPage{}, fmt.Errorf("scan telemetry record: %w", err)
		}
		if len(page.Records) == request.Limit {
			cursorValue, err := encodeRecordCursor(page.Records[len(page.Records)-1])
			if err != nil {
				return RecordPage{}, err
			}
			page.NextCursor = &cursorValue
			break
		}
		page.Records = append(page.Records, record)
	}
	if err := rows.Err(); err != nil {
		return RecordPage{}, fmt.Errorf("iterate telemetry records: %w", err)
	}
	return page, nil
}

// ExportRecordsJSON streams records without loading the retained archive into
// memory. Each item wraps the exact stored record JSON in the "record" field.
func (q *ArchiveQuery) ExportRecordsJSON(ctx context.Context, request RecordQuery, writer io.Writer) error {
	if q == nil || q.db == nil || writer == nil {
		return fmt.Errorf("%w: export dependency is unavailable", ErrInvalidQuery)
	}
	if err := ValidateRecordQuery(request); err != nil {
		return err
	}
	request.DeviceID = strings.TrimSpace(request.DeviceID)
	cursor, _ := decodeRecordCursor(request.Cursor)
	statement, args, err := recordQuerySQL(request, cursor, 0)
	if err != nil {
		return err
	}
	rows, err := q.db.QueryContext(ctx, statement, args...)
	if err != nil {
		return fmt.Errorf("export telemetry records: %w", err)
	}
	defer rows.Close()

	if _, err := fmt.Fprintf(writer, `{"device_id":%q,"records":[`, request.DeviceID); err != nil {
		return err
	}
	first := true
	for rows.Next() {
		record, err := scanArchiveRecord(rows)
		if err != nil {
			return fmt.Errorf("scan telemetry export record: %w", err)
		}
		if !first {
			if _, err := io.WriteString(writer, ","); err != nil {
				return err
			}
		}
		first = false
		if err := writeArchiveRecordJSON(writer, record); err != nil {
			return err
		}
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("iterate telemetry export records: %w", err)
	}
	_, err = io.WriteString(writer, "]}")
	return err
}

// writeArchiveRecordJSON writes the documented archive-record envelope while
// inserting the persisted record bytes directly. Encoding an ArchiveRecord
// with encoding/json would HTML-escape strings inside RawMessage (for example
// < and >) and would therefore change the exact evidence captured at ingest.
func writeArchiveRecordJSON(writer io.Writer, record ArchiveRecord) error {
	if writer == nil || !json.Valid(record.Record) {
		return fmt.Errorf("invalid archive record bytes")
	}
	metadata := struct {
		DeviceID    string  `json:"device_id"`
		RecordID    string  `json:"record_id"`
		RecordHash  string  `json:"record_hash"`
		Kind        string  `json:"kind"`
		BootID      string  `json:"boot_id"`
		Sequence    string  `json:"sequence"`
		MonotonicUS string  `json:"monotonic_us"`
		RecordedAt  *string `json:"recorded_at"`
		TimeQuality string  `json:"time_quality"`
		Stream      *string `json:"stream,omitempty"`
		SensorID    *string `json:"sensor_id,omitempty"`
		Quality     *string `json:"quality,omitempty"`
	}{
		DeviceID: record.DeviceID, RecordID: record.RecordID,
		RecordHash: record.RecordHash, Kind: record.Kind,
		BootID: record.BootID, Sequence: record.Sequence,
		MonotonicUS: record.MonotonicUS, RecordedAt: record.RecordedAt,
		TimeQuality: record.TimeQuality, Stream: record.Stream,
		SensorID: record.SensorID, Quality: record.Quality,
	}
	encoded, err := json.Marshal(metadata)
	if err != nil {
		return err
	}
	if len(encoded) == 0 || encoded[len(encoded)-1] != '}' {
		return fmt.Errorf("invalid archive record envelope")
	}
	if _, err := writer.Write(encoded[:len(encoded)-1]); err != nil {
		return err
	}
	if _, err := io.WriteString(writer, `,"record":`); err != nil {
		return err
	}
	if _, err := writer.Write(record.Record); err != nil {
		return err
	}
	_, err = io.WriteString(writer, "}")
	return err
}

func recordQuerySQL(request RecordQuery, cursor recordCursor, limit int) (string, []any, error) {
	clauses := []string{"device_id = ?"}
	args := []any{request.DeviceID}
	addTextFilter := func(column, value string) {
		if strings.TrimSpace(value) == "" {
			return
		}
		clauses = append(clauses, column+" = ?")
		args = append(args, strings.TrimSpace(value))
	}
	addTextFilter("kind", request.Kind)
	addTextFilter("stream", request.Stream)
	addTextFilter("sensor_id", request.SensorID)
	addTextFilter("quality", request.Quality)
	if request.UTCNull != nil {
		if *request.UTCNull {
			clauses = append(clauses, "recorded_at IS NULL")
		} else {
			clauses = append(clauses, "recorded_at IS NOT NULL")
		}
	}
	if cursor.DeviceID != "" {
		sortKey, err := sequenceSortKey(cursor.Sequence)
		if err != nil {
			return "", nil, err
		}
		clauses = append(clauses, `(boot_id > ? OR (boot_id = ? AND sequence_sort > ?) OR (boot_id = ? AND sequence_sort = ? AND record_id > ?))`)
		args = append(args, cursor.BootID, cursor.BootID, sortKey, cursor.BootID, sortKey, cursor.RecordID)
	}
	statement := `
SELECT device_id, record_id, record_hash, kind, boot_id, sequence_decimal,
       monotonic_us, recorded_at, time_quality, stream, sensor_id, quality,
       record_bytes
FROM telemetry_records
WHERE ` + strings.Join(clauses, " AND ") + `
ORDER BY boot_id ASC, sequence_sort ASC, record_id ASC`
	if limit > 0 {
		statement += `
LIMIT ?`
		args = append(args, limit)
	}
	return statement, args, nil
}

type archiveRecordScanner interface {
	Scan(dest ...any) error
}

func scanArchiveRecord(scanner archiveRecordScanner) (ArchiveRecord, error) {
	var record ArchiveRecord
	var recordedAt, stream, sensorID, quality sql.NullString
	var raw []byte
	if err := scanner.Scan(
		&record.DeviceID,
		&record.RecordID,
		&record.RecordHash,
		&record.Kind,
		&record.BootID,
		&record.Sequence,
		&record.MonotonicUS,
		&recordedAt,
		&record.TimeQuality,
		&stream,
		&sensorID,
		&quality,
		&raw,
	); err != nil {
		return ArchiveRecord{}, err
	}
	if recordedAt.Valid {
		record.RecordedAt = &recordedAt.String
	}
	if stream.Valid {
		record.Stream = &stream.String
	}
	if sensorID.Valid {
		record.SensorID = &sensorID.String
	}
	if quality.Valid {
		record.Quality = &quality.String
	}
	record.Record = append(json.RawMessage(nil), raw...)
	return record, nil
}

func sequenceSortKey(value string) (string, error) {
	key, err := telemetry.SequenceSortKey(value)
	if err != nil {
		return "", fmt.Errorf("%w: invalid sequence", ErrInvalidQuery)
	}
	return key, nil
}
