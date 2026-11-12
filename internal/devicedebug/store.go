package devicedebug

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"reflect"
	"strconv"
	"sync"
	"time"
)

var (
	ErrInvalid  = errors.New("DEBUG_STORE_INVALID")
	ErrConflict = errors.New("DEBUG_CONTENT_CONFLICT")
	ErrNotFound = errors.New("DEBUG_DEVICE_NOT_FOUND")
	ErrStorage  = errors.New("DEBUG_STORE_UNAVAILABLE")
)

type Policy struct {
	Retention      time.Duration
	PerDeviceLimit int
	GlobalLimit    int
}

func DefaultPolicy() Policy {
	return Policy{Retention: 24 * time.Hour, PerDeviceLimit: 2000, GlobalLimit: 20000}
}

type Store struct {
	mu     sync.Mutex
	db     *sql.DB
	policy Policy
	clock  func() time.Time
}

func NewStore(db *sql.DB, policy Policy) (*Store, error) {
	if db == nil || policy.Retention <= 0 || policy.PerDeviceLimit <= 0 || policy.GlobalLimit < policy.PerDeviceLimit {
		return nil, ErrInvalid
	}
	return &Store{db: db, policy: policy, clock: time.Now}, nil
}

// Configure the clock before sharing the store with concurrent callers.
func (s *Store) WithClock(clock func() time.Time) *Store {
	if clock != nil {
		s.clock = clock
	}
	return s
}

func formatU64(value uint64) string { return strconv.FormatUint(value, 10) }

func formatServerTime(value time.Time) string {
	return value.UTC().Format("2006-01-02T15:04:05.000000000Z")
}

func (s *Store) Ingest(ctx context.Context, upload Upload, exactBody []byte) (UploadAck, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var ack UploadAck
	parsed, err := DecodeUpload(exactBody)
	if err != nil || !reflect.DeepEqual(parsed, upload) || len(upload.Events) == 0 || len(upload.Events) > 8 || len(upload.Events) > s.policy.PerDeviceLimit {
		return ack, ErrInvalid
	}
	var registered string
	if err := s.db.QueryRowContext(ctx, `SELECT device_code FROM devices WHERE device_code = ?`, upload.DeviceID).Scan(&registered); err != nil || registered != upload.DeviceID {
		if errors.Is(err, sql.ErrNoRows) {
			return ack, ErrNotFound
		}
		return ack, ErrStorage
	}
	requestHash := sha256.Sum256(exactBody)
	now := s.clock().UTC()
	receivedAt := formatServerTime(now)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return ack, ErrStorage
	}
	defer tx.Rollback()
	stored, deduplicated := uint8(0), uint8(0)
	protectedIDs := make([]int64, 0, len(upload.Events))
	expiry := formatServerTime(now.Add(-s.policy.Retention))
	for _, event := range upload.Events {
		canonical, marshalErr := json.Marshal(event)
		if marshalErr != nil {
			return ack, ErrInvalid
		}
		var existingBuild, existingJSON, existingReceipt string
		var existingID int64
		err := tx.QueryRowContext(ctx, `SELECT ingestion_id,build_id,event_json,received_at FROM device_debug_history WHERE device_code=? AND boot_id=? AND sequence_text=?`, upload.DeviceID, upload.BootID, event.Sequence).Scan(&existingID, &existingBuild, &existingJSON, &existingReceipt)
		if err == nil {
			if existingReceipt > expiry {
				if existingBuild != upload.BuildID || existingJSON != string(canonical) {
					return ack, ErrConflict
				}
				deduplicated++
				protectedIDs = append(protectedIDs, existingID)
				continue
			}
			if err := markPruned(ctx, tx, upload.DeviceID, existingID); err != nil {
				return ack, ErrStorage
			}
			if _, err := tx.ExecContext(ctx, `DELETE FROM device_debug_history WHERE ingestion_id=?`, existingID); err != nil {
				return ack, ErrStorage
			}
		}
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return ack, ErrStorage
		}
		_, err = tx.ExecContext(ctx, `INSERT INTO device_debug_history
			(device_code,boot_id,sequence_text,event_json,build_id,received_at,snapshot_level,captured_monotonic_us,oldest_sequence,overwritten,delivery_lost)
			VALUES (?,?,?,?,?,?,?,?,?,?,?)`, upload.DeviceID, upload.BootID, event.Sequence, string(canonical), upload.BuildID, receivedAt, upload.Level, upload.CapturedMonotonicUS, upload.OldestSequence, upload.Overwritten, upload.DeliveryLost)
		if err != nil {
			return ack, ErrStorage
		}
		stored++
		var ingestionID int64
		if err := tx.QueryRowContext(ctx, `SELECT last_insert_rowid()`).Scan(&ingestionID); err != nil {
			return ack, ErrStorage
		}
		protectedIDs = append(protectedIDs, ingestionID)
	}
	if err := s.prune(ctx, tx, now, upload.DeviceID, protectedIDs); err != nil {
		return ack, ErrStorage
	}
	if err := tx.Commit(); err != nil {
		return ack, ErrStorage
	}
	return UploadAck{SchemaVersion: 1, Status: "DURABLE", DeviceID: upload.DeviceID, BootID: upload.BootID,
		FirstSequence: upload.Events[0].Sequence, LastSequence: upload.Events[len(upload.Events)-1].Sequence,
		StoredCount: stored, DeduplicatedCount: deduplicated, RequestSHA256: hex.EncodeToString(requestHash[:])}, nil
}

func (s *Store) Query(ctx context.Context, deviceID, after string, limit int) (HistoryPage, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	page := HistoryPage{DeviceID: deviceID, Events: []HistoryEvent{}, NextAfter: after, OldestAvailable: "0"}
	cursor, err := parseCursor(after)
	if err != nil || deviceID == "" || limit < 1 || limit > 100 {
		return HistoryPage{}, ErrInvalid
	}
	var registered string
	if err := s.db.QueryRowContext(ctx, `SELECT device_code FROM devices WHERE device_code=?`, deviceID).Scan(&registered); err != nil || registered != deviceID {
		if errors.Is(err, sql.ErrNoRows) {
			return HistoryPage{}, ErrNotFound
		}
		return HistoryPage{}, ErrStorage
	}
	now := s.clock().UTC()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return HistoryPage{}, ErrStorage
	}
	defer tx.Rollback()
	if err := s.prune(ctx, tx, now, "", nil); err != nil {
		return HistoryPage{}, ErrStorage
	}
	var watermark sql.NullInt64
	if err := tx.QueryRowContext(ctx, `SELECT pruned_through FROM device_debug_watermarks WHERE device_code=?`, deviceID).Scan(&watermark); err != nil && !errors.Is(err, sql.ErrNoRows) {
		return HistoryPage{}, ErrStorage
	}
	page.RetentionGap = watermark.Valid && cursor < watermark.Int64
	var oldest sql.NullInt64
	cutoff := formatServerTime(now.Add(-s.policy.Retention))
	if err := tx.QueryRowContext(ctx, `SELECT MIN(ingestion_id) FROM device_debug_history WHERE device_code=? AND received_at>?`, deviceID, cutoff).Scan(&oldest); err != nil {
		return HistoryPage{}, ErrStorage
	}
	if oldest.Valid {
		page.OldestAvailable = strconv.FormatInt(oldest.Int64, 10)
	}
	rows, err := tx.QueryContext(ctx, `SELECT ingestion_id,received_at,boot_id,build_id,snapshot_level,captured_monotonic_us,oldest_sequence,overwritten,delivery_lost,event_json
		FROM device_debug_history WHERE device_code=? AND ingestion_id>? AND received_at>? ORDER BY ingestion_id ASC LIMIT ?`, deviceID, cursor, cutoff, limit+1)
	if err != nil {
		return HistoryPage{}, ErrStorage
	}
	for rows.Next() {
		var id int64
		var event HistoryEvent
		var content string
		if err := rows.Scan(&id, &event.ReceivedAt, &event.BootID, &event.BuildID, &event.SnapshotLevel, &event.CapturedMonotonicUS, &event.OldestSequence, &event.Overwritten, &event.DeliveryLost, &content); err != nil {
			rows.Close()
			return HistoryPage{}, ErrStorage
		}
		if err := json.Unmarshal([]byte(content), &event.DebugEvent); err != nil {
			rows.Close()
			return HistoryPage{}, ErrStorage
		}
		event.IngestionID = strconv.FormatInt(id, 10)
		page.Events = append(page.Events, event)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return HistoryPage{}, ErrStorage
	}
	if err := rows.Close(); err != nil {
		return HistoryPage{}, ErrStorage
	}
	if len(page.Events) > limit {
		page.Events = page.Events[:limit]
		page.HasMore = true
	}
	if len(page.Events) > 0 {
		page.NextAfter = page.Events[len(page.Events)-1].IngestionID
	}
	if err := tx.Commit(); err != nil {
		return HistoryPage{}, ErrStorage
	}
	return page, nil
}

func parseCursor(value string) (int64, error) {
	if value == "0" {
		return 0, nil
	}
	if value == "" || (len(value) > 1 && value[0] == '0') {
		return 0, ErrInvalid
	}
	for i := 0; i < len(value); i++ {
		if value[i] < '0' || value[i] > '9' {
			return 0, ErrInvalid
		}
	}
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil || parsed <= 0 {
		return 0, ErrInvalid
	}
	return parsed, nil
}

func (s *Store) prune(ctx context.Context, tx *sql.Tx, now time.Time, protectedDevice string, protectedIDs []int64) error {
	cutoff := formatServerTime(now.Add(-s.policy.Retention))
	if err := pruneRows(ctx, tx, `SELECT ingestion_id,device_code FROM device_debug_history WHERE received_at<=? ORDER BY ingestion_id`, cutoff); err != nil {
		return err
	}
	perDeviceLimit := s.policy.PerDeviceLimit - len(protectedIDs)
	if protectedDevice != "" && perDeviceLimit < 0 {
		return ErrInvalid
	}
	perDeviceQuery := `SELECT ingestion_id,device_code FROM (
		SELECT ingestion_id,device_code,ROW_NUMBER() OVER (PARTITION BY device_code ORDER BY ingestion_id DESC) AS recent_rank
		FROM device_debug_history
	) WHERE recent_rank > CASE WHEN device_code=? THEN ? ELSE ? END ORDER BY ingestion_id`
	perDeviceArgs := []any{"", 0, s.policy.PerDeviceLimit}
	if len(protectedIDs) > 0 {
		perDeviceArgs = nil
		perDeviceQuery = `SELECT ingestion_id,device_code FROM (
			SELECT ingestion_id,device_code,ROW_NUMBER() OVER (PARTITION BY device_code ORDER BY ingestion_id DESC) AS recent_rank
			FROM device_debug_history WHERE ingestion_id NOT IN (` + placeholders(len(protectedIDs)) + `)
		) WHERE recent_rank > CASE WHEN device_code=? THEN ? ELSE ? END ORDER BY ingestion_id`
		for _, id := range protectedIDs {
			perDeviceArgs = append(perDeviceArgs, id)
		}
		perDeviceArgs = append(perDeviceArgs, protectedDevice, perDeviceLimit, s.policy.PerDeviceLimit)
	}
	if err := pruneRows(ctx, tx, perDeviceQuery, perDeviceArgs...); err != nil {
		return err
	}
	var total int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM device_debug_history`).Scan(&total); err != nil {
		return err
	}
	if excess := total - s.policy.GlobalLimit; excess > 0 {
		query := `SELECT ingestion_id,device_code FROM device_debug_history`
		args := []any{}
		eligible := `SELECT COUNT(*) FROM device_debug_history`
		eligibleArgs := []any{}
		if len(protectedIDs) > 0 {
			query += ` WHERE ingestion_id NOT IN (` + placeholders(len(protectedIDs)) + `)`
			eligible += ` WHERE ingestion_id NOT IN (` + placeholders(len(protectedIDs)) + `)`
			for _, id := range protectedIDs {
				args = append(args, id)
				eligibleArgs = append(eligibleArgs, id)
			}
		}
		var eligibleCount int
		if err := tx.QueryRowContext(ctx, eligible, eligibleArgs...).Scan(&eligibleCount); err != nil {
			return err
		}
		if eligibleCount < excess {
			return ErrInvalid
		}
		query += ` ORDER BY ingestion_id LIMIT ?`
		args = append(args, excess)
		if err := pruneRows(ctx, tx, query, args...); err != nil {
			return err
		}
	}
	return nil
}

func markPruned(ctx context.Context, tx *sql.Tx, deviceCode string, ingestionID int64) error {
	_, err := tx.ExecContext(ctx, `INSERT INTO device_debug_watermarks(device_code,pruned_through) VALUES (?,?) ON CONFLICT(device_code) DO UPDATE SET pruned_through=MAX(pruned_through,excluded.pruned_through)`, deviceCode, ingestionID)
	return err
}

func placeholders(count int) string {
	result := "?"
	for i := 1; i < count; i++ {
		result += ",?"
	}
	return result
}

func pruneRows(ctx context.Context, tx *sql.Tx, query string, args ...any) error {
	rows, err := tx.QueryContext(ctx, query, args...)
	if err != nil {
		return err
	}
	type row struct {
		id   int64
		code string
	}
	var pruned []row
	for rows.Next() {
		var item row
		if err := rows.Scan(&item.id, &item.code); err != nil {
			rows.Close()
			return err
		}
		pruned = append(pruned, item)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	if err := rows.Close(); err != nil {
		return err
	}
	for _, item := range pruned {
		if err := markPruned(ctx, tx, item.code, item.id); err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `DELETE FROM device_debug_history WHERE ingestion_id=?`, item.id); err != nil {
			return err
		}
	}
	return nil
}
