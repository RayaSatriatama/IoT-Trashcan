// Package history provides the shared audit-reading query and CSV projection.
package history

import (
	"context"
	"database/sql"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/device"
)

var ErrInvalidFilter = errors.New("invalid history filter")

const (
	DefaultPageSize = 50
	MaxPageSize     = 200
	MaxTrendBuckets = 120
)

const effectiveStatusSQL = `CASE
	WHEN dec.safety_state = 'Safety Alert' THEN 'Safety Alert'
	ELSE dec.fis_status
END`

// Filters is the common filter contract for history and CSV export. From is
// inclusive and To is exclusive. Empty strings and nil times mean unset.
type Filters struct {
	From       *time.Time
	To         *time.Time
	LocationID string
	DeviceID   string
	Status     string
}

// FilterState is the stable response representation of the applied filters.
type FilterState struct {
	From       *string `json:"from"`
	To         *string `json:"to"`
	LocationID *string `json:"location_id"`
	DeviceID   *string `json:"device_id"`
	Status     *string `json:"status"`
}

// Cursor identifies the last record of a history page. The next page asks for
// rows strictly older than this tuple, which remains stable while new readings
// are inserted at the head of the history stream.
type Cursor struct {
	RecordedAt string `json:"recorded_at"`
	ReadingID  int64  `json:"reading_id"`
}

// PageOptions controls bounded history reads. CSV export intentionally does
// not use this type so filtered exports remain complete.
type PageOptions struct {
	Limit  int
	Before *Cursor
}

// PageResult contains one bounded history page plus the cursor for older rows.
type PageResult struct {
	Items      []Record
	HasMore    bool
	Next       *Cursor
	TotalCount int
}

// Summary contains complete filtered counts, independent of pagination.
type Summary struct {
	Total       int `json:"total"`
	Normal      int `json:"normal"`
	Waspada     int `json:"waspada"`
	Pickup      int `json:"pickup"`
	SafetyAlert int `json:"safety_alert"`
	DataInvalid int `json:"data_invalid"`
}

// TrendBucket contains one complete local calendar bucket of filtered data.
type TrendBucket struct {
	Start       string `json:"start"`
	Label       string `json:"label"`
	Total       int    `json:"total"`
	Normal      int    `json:"normal"`
	Waspada     int    `json:"waspada"`
	Pickup      int    `json:"pickup"`
	SafetyAlert int    `json:"safety_alert"`
	DataInvalid int    `json:"data_invalid"`
}

// Trend is a bounded time series over the filtered History scope.
type Trend struct {
	ScopeFrom     string        `json:"scope_from"`
	ScopeTo       string        `json:"scope_to"`
	Granularity   string        `json:"granularity"`
	DefaultWindow bool          `json:"default_window"`
	Truncated     bool          `json:"truncated"`
	Buckets       []TrendBucket `json:"buckets"`
}

// ViewResult keeps a History page and its complete analytics in one snapshot.
type ViewResult struct {
	Page    PageResult
	Summary Summary
	Trend   Trend
}

type queryRunner interface {
	QueryContext(context.Context, string, ...any) (*sql.Rows, error)
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

func (filters Filters) State() FilterState {
	state := FilterState{}
	if filters.From != nil {
		value := filters.From.UTC().Format(time.RFC3339Nano)
		state.From = &value
	}
	if filters.To != nil {
		value := filters.To.UTC().Format(time.RFC3339Nano)
		state.To = &value
	}
	if filters.LocationID != "" {
		state.LocationID = stringPointer(filters.LocationID)
	}
	if filters.DeviceID != "" {
		state.DeviceID = stringPointer(filters.DeviceID)
	}
	if filters.Status != "" {
		state.Status = stringPointer(filters.Status)
	}
	return state
}

// ParseFilters validates query parameters shared by history and export.
func ParseFilters(values url.Values) (Filters, error) {
	filters := Filters{
		LocationID: strings.TrimSpace(values.Get("location_id")),
		DeviceID:   strings.TrimSpace(values.Get("device_id")),
		Status:     strings.TrimSpace(values.Get("status")),
	}
	for name, destination := range map[string]**time.Time{"from": &filters.From, "to": &filters.To} {
		value := strings.TrimSpace(values.Get(name))
		if value == "" {
			continue
		}
		parsed, err := time.Parse(time.RFC3339, value)
		if err != nil {
			return Filters{}, fmt.Errorf("%w: %s must be RFC3339", ErrInvalidFilter, name)
		}
		parsed = parsed.UTC()
		*destination = &parsed
	}
	if filters.From != nil && filters.To != nil && !filters.To.After(*filters.From) {
		return Filters{}, fmt.Errorf("%w: to must be after from", ErrInvalidFilter)
	}
	if filters.Status != "" && !isPublicStatus(filters.Status) {
		return Filters{}, fmt.Errorf("%w: invalid status", ErrInvalidFilter)
	}
	return filters, nil
}

// Record is the audit-safe history row. No raw payload or credential material
// is retained in this DTO; gas_raw is extracted from the sensor payload.
type Record struct {
	ReadingID         int64           `json:"reading_id"`
	RecordedAt        string          `json:"recorded_at"`
	LocationID        string          `json:"location_id"`
	LocationName      string          `json:"location_name"`
	DeviceID          string          `json:"device_id"`
	DeviceCode        string          `json:"device_code"`
	PlacementLabel    *string         `json:"placement_label"`
	SlotID            string          `json:"slot_id"`
	Stream            string          `json:"stream"`
	Fill              *float64        `json:"fill"`
	Weight            *float64        `json:"weight"`
	WeightUtilization *float64        `json:"weight_utilization"`
	GasRaw            *float64        `json:"gas_raw"`
	OGI               *float64        `json:"ogi"`
	AccumulationHours *float64        `json:"accumulation_hours"`
	Temperature       *float64        `json:"temperature"`
	FISScore          *float64        `json:"fis_score"`
	FISStatus         *string         `json:"fis_status"`
	FISVersion        *string         `json:"fis_version"`
	DominantReason    *string         `json:"dominant_reason"`
	SafetyState       *string         `json:"safety_state"`
	SensorHealth      json.RawMessage `json:"sensor_health"`
}

// Query returns the complete filtered audit set in deterministic newest-first
// order. CSV export uses this path so export semantics remain complete.
func Query(ctx context.Context, db *sql.DB, filters Filters) ([]Record, error) {
	if db == nil {
		return nil, errors.New("history database is unavailable")
	}
	return queryRecords(ctx, db, filters, nil)
}

// QueryPage returns a bounded page suitable for the interactive History UI.
// It fetches one extra row to determine whether older data is available.
func QueryPage(ctx context.Context, db *sql.DB, filters Filters, options PageOptions) (PageResult, error) {
	if db == nil {
		return PageResult{}, errors.New("history database is unavailable")
	}
	result, err := queryPage(ctx, db, filters, options)
	if err != nil {
		return PageResult{}, err
	}
	totalCount, err := countRecords(ctx, db, filters)
	if err != nil {
		return PageResult{}, err
	}
	result.TotalCount = totalCount
	return result, nil
}

// QueryView reads a page, its complete filtered summary, and its trend from
// one read-only snapshot so concurrent ingests cannot mix response states.
func QueryView(ctx context.Context, db *sql.DB, filters Filters, page PageOptions, now time.Time) (ViewResult, error) {
	if db == nil {
		return ViewResult{}, errors.New("history database is unavailable")
	}
	tx, err := db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return ViewResult{}, err
	}
	defer tx.Rollback()

	pageResult, err := queryPage(ctx, tx, filters, page)
	if err != nil {
		return ViewResult{}, err
	}
	summary, err := querySummary(ctx, tx, filters)
	if err != nil {
		return ViewResult{}, err
	}
	trend, err := queryTrend(ctx, tx, filters, now)
	if err != nil {
		return ViewResult{}, err
	}
	pageResult.TotalCount = summary.Total
	if err := tx.Commit(); err != nil {
		return ViewResult{}, err
	}
	return ViewResult{Page: pageResult, Summary: summary, Trend: trend}, nil
}

func queryPage(ctx context.Context, runner queryRunner, filters Filters, options PageOptions) (PageResult, error) {
	limit, err := pageLimit(options)
	if err != nil {
		return PageResult{}, err
	}
	queryOptions := PageOptions{Limit: limit + 1, Before: options.Before}
	items, err := queryRecords(ctx, runner, filters, &queryOptions)
	if err != nil {
		return PageResult{}, err
	}
	hasMore := len(items) > limit
	if hasMore {
		items = items[:limit]
	}
	var next *Cursor
	if hasMore && len(items) > 0 {
		last := items[len(items)-1]
		next = &Cursor{RecordedAt: last.RecordedAt, ReadingID: last.ReadingID}
	}
	return PageResult{Items: items, HasMore: hasMore, Next: next}, nil
}

func pageLimit(options PageOptions) (int, error) {
	limit := options.Limit
	if limit == 0 {
		limit = DefaultPageSize
	}
	if limit < 1 || limit > MaxPageSize {
		return 0, fmt.Errorf("%w: limit must be between 1 and %d", ErrInvalidFilter, MaxPageSize)
	}
	if options.Before != nil {
		if strings.TrimSpace(options.Before.RecordedAt) == "" || options.Before.ReadingID <= 0 {
			return 0, fmt.Errorf("%w: invalid history cursor", ErrInvalidFilter)
		}
		if _, err := time.Parse(time.RFC3339Nano, options.Before.RecordedAt); err != nil {
			return 0, fmt.Errorf("%w: invalid history cursor", ErrInvalidFilter)
		}
	}
	return limit, nil
}

func historyWhere(filters Filters, before *Cursor) ([]string, []any) {
	where := []string{"1 = 1"}
	args := make([]any, 0, 9)
	if filters.From != nil {
		where = append(where, "r.recorded_at >= ?")
		args = append(args, filters.From.UTC().Format(time.RFC3339Nano))
	}
	if filters.To != nil {
		where = append(where, "r.recorded_at < ?")
		args = append(args, filters.To.UTC().Format(time.RFC3339Nano))
	}
	if filters.LocationID != "" {
		where = append(where, "l.id = ?")
		args = append(args, filters.LocationID)
	}
	if filters.DeviceID != "" {
		where = append(where, "(d.id = ? OR d.device_code = ?)")
		args = append(args, filters.DeviceID, filters.DeviceID)
	}
	if filters.Status != "" {
		where = append(where, effectiveStatusSQL+" = ?")
		args = append(args, filters.Status)
	}
	if before != nil {
		where = append(where, "(r.recorded_at < ? OR (r.recorded_at = ? AND r.id < ?))")
		args = append(args, before.RecordedAt, before.RecordedAt, before.ReadingID)
	}
	return where, args
}

func countRecords(ctx context.Context, runner queryRunner, filters Filters) (int, error) {
	where, args := historyWhere(filters, nil)
	query := `
		SELECT COUNT(*)
		FROM readings r
		JOIN slots s ON s.id = r.slot_id
		JOIN devices d ON d.id = s.device_id
		JOIN locations l ON l.id = d.location_id
		LEFT JOIN decisions dec ON dec.reading_id = r.id
		WHERE ` + strings.Join(where, " AND ")
	var count int
	if err := runner.QueryRowContext(ctx, query, args...).Scan(&count); err != nil {
		return 0, err
	}
	return count, nil
}

func querySummary(ctx context.Context, runner queryRunner, filters Filters) (Summary, error) {
	where, args := historyWhere(filters, nil)
	query := `
		SELECT COUNT(*),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Normal' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Waspada' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Angkat' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Safety Alert' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Data Invalid' THEN 1 ELSE 0 END), 0)
		FROM readings r
		JOIN slots s ON s.id = r.slot_id
		JOIN devices d ON d.id = s.device_id
		JOIN locations l ON l.id = d.location_id
		LEFT JOIN decisions dec ON dec.reading_id = r.id
		WHERE ` + strings.Join(where, " AND ")
	var summary Summary
	if err := runner.QueryRowContext(ctx, query, args...).Scan(
		&summary.Total,
		&summary.Normal,
		&summary.Waspada,
		&summary.Pickup,
		&summary.SafetyAlert,
		&summary.DataInvalid,
	); err != nil {
		return Summary{}, err
	}
	return summary, nil
}

var jakartaLocation = time.FixedZone("Asia/Jakarta", 7*60*60)

func queryTrend(ctx context.Context, runner queryRunner, filters Filters, now time.Time) (Trend, error) {
	scopeFrom, scopeTo, defaultWindow := trendScope(filters, now)
	granularity := trendGranularity(scopeFrom, scopeTo)
	buckets, truncated := trendBuckets(scopeFrom, scopeTo, granularity)
	counts, err := queryTrendCounts(ctx, runner, filters, scopeFrom, scopeTo, granularity)
	if err != nil {
		return Trend{}, err
	}
	for index := range buckets {
		if summary, ok := counts[buckets[index].Start]; ok {
			buckets[index].Total = summary.Total
			buckets[index].Normal = summary.Normal
			buckets[index].Waspada = summary.Waspada
			buckets[index].Pickup = summary.Pickup
			buckets[index].SafetyAlert = summary.SafetyAlert
			buckets[index].DataInvalid = summary.DataInvalid
		}
	}
	return Trend{
		ScopeFrom:     scopeFrom.UTC().Format(time.RFC3339),
		ScopeTo:       scopeTo.UTC().Format(time.RFC3339),
		Granularity:   granularity,
		DefaultWindow: defaultWindow,
		Truncated:     truncated,
		Buckets:       buckets,
	}, nil
}

func trendScope(filters Filters, now time.Time) (time.Time, time.Time, bool) {
	today := startOfLocalDay(now.In(jakartaLocation))
	if filters.From == nil && filters.To == nil {
		return today.AddDate(0, 0, -29), today.AddDate(0, 0, 1), true
	}
	if filters.From == nil {
		to := filters.To.In(jakartaLocation)
		return to.AddDate(0, 0, -30), to, false
	}
	from := filters.From.In(jakartaLocation)
	if filters.To != nil {
		return from, filters.To.In(jakartaLocation), false
	}
	to := now.In(jakartaLocation)
	if from.After(to) {
		to = from
	}
	return from, to, false
}

func trendGranularity(scopeFrom, scopeTo time.Time) string {
	days := int(scopeTo.Sub(scopeFrom).Hours() / 24)
	switch {
	case days <= 31:
		return "day"
	case days <= 180:
		return "week"
	default:
		return "month"
	}
}

func trendBuckets(scopeFrom, scopeTo time.Time, granularity string) ([]TrendBucket, bool) {
	start := trendBucketStart(scopeFrom, granularity)
	buckets := make([]TrendBucket, 0)
	for start.Before(scopeTo) {
		buckets = append(buckets, TrendBucket{
			Start: start.UTC().Format(time.RFC3339),
			Label: trendBucketLabel(start, granularity),
		})
		start = nextTrendBucket(start, granularity)
	}
	truncated := len(buckets) > MaxTrendBuckets
	if truncated {
		buckets = buckets[len(buckets)-MaxTrendBuckets:]
	}
	return buckets, truncated
}

func trendBucketStart(value time.Time, granularity string) time.Time {
	value = value.In(jakartaLocation)
	switch granularity {
	case "week":
		start := startOfLocalDay(value)
		return start.AddDate(0, 0, -((int(start.Weekday()) + 6) % 7))
	case "month":
		return time.Date(value.Year(), value.Month(), 1, 0, 0, 0, 0, jakartaLocation)
	default:
		return startOfLocalDay(value)
	}
}

func startOfLocalDay(value time.Time) time.Time {
	return time.Date(value.Year(), value.Month(), value.Day(), 0, 0, 0, 0, jakartaLocation)
}

func nextTrendBucket(start time.Time, granularity string) time.Time {
	switch granularity {
	case "week":
		return start.AddDate(0, 0, 7)
	case "month":
		return start.AddDate(0, 1, 0)
	default:
		return start.AddDate(0, 0, 1)
	}
}

func trendBucketLabel(start time.Time, granularity string) string {
	switch granularity {
	case "month":
		return start.Format("Jan 2006")
	default:
		return start.Format("02 Jan")
	}
}

func queryTrendCounts(ctx context.Context, runner queryRunner, filters Filters, scopeFrom, scopeTo time.Time, granularity string) (map[string]Summary, error) {
	where, args := historyWhere(filters, nil)
	where = append(where, "r.recorded_at >= ?", "r.recorded_at < ?")
	args = append(args, scopeFrom.UTC().Format(time.RFC3339Nano), scopeTo.UTC().Format(time.RFC3339Nano))
	bucketSQL := trendBucketSQL(granularity)
	query := `
		SELECT ` + bucketSQL + `,
			COUNT(*),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Normal' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Waspada' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Angkat' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Safety Alert' THEN 1 ELSE 0 END), 0),
			COALESCE(SUM(CASE WHEN ` + effectiveStatusSQL + ` = 'Data Invalid' THEN 1 ELSE 0 END), 0)
		FROM readings r
		JOIN slots s ON s.id = r.slot_id
		JOIN devices d ON d.id = s.device_id
		JOIN locations l ON l.id = d.location_id
		LEFT JOIN decisions dec ON dec.reading_id = r.id
		WHERE ` + strings.Join(where, " AND ") + `
		GROUP BY ` + bucketSQL
	rows, err := runner.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	counts := make(map[string]Summary)
	for rows.Next() {
		var key string
		var summary Summary
		if err := rows.Scan(&key, &summary.Total, &summary.Normal, &summary.Waspada, &summary.Pickup, &summary.SafetyAlert, &summary.DataInvalid); err != nil {
			return nil, err
		}
		start, err := time.ParseInLocation(trendBucketLayout(granularity), key, jakartaLocation)
		if err != nil {
			return nil, err
		}
		counts[start.UTC().Format(time.RFC3339)] = summary
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return counts, nil
}

func trendBucketSQL(granularity string) string {
	switch granularity {
	case "week":
		return "date(r.recorded_at, '+7 hours', '-6 days', 'weekday 1')"
	case "month":
		return "strftime('%Y-%m', r.recorded_at, '+7 hours')"
	default:
		return "date(r.recorded_at, '+7 hours')"
	}
}

func trendBucketLayout(granularity string) string {
	if granularity == "month" {
		return "2006-01"
	}
	return "2006-01-02"
}

func queryRecords(ctx context.Context, runner queryRunner, filters Filters, page *PageOptions) ([]Record, error) {
	var before *Cursor
	if page != nil {
		before = page.Before
	}
	where, args := historyWhere(filters, before)

	query := `
		SELECT r.id, r.recorded_at,
			l.id, l.name,
			d.id, d.device_code, d.placement_label,
			s.id, s.waste_stream,
			r.raw_payload, r.fill, r.weight, r.weight_utilization,
			r.ogi, r.accumulation, r.temperature, r.sensor_health,
			dec.fis_score, dec.fis_status, dec.fis_version,
			dec.dominant_reason, dec.safety_state
		FROM readings r
		JOIN slots s ON s.id = r.slot_id
		JOIN devices d ON d.id = s.device_id
		JOIN locations l ON l.id = d.location_id
		LEFT JOIN decisions dec ON dec.reading_id = r.id
		WHERE ` + strings.Join(where, " AND ") + `
		ORDER BY r.recorded_at DESC, r.id DESC`
	if page != nil {
		query += "\n\t\tLIMIT ?"
		args = append(args, page.Limit)
	}

	rows, err := runner.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]Record, 0)
	for rows.Next() {
		var item Record
		var placement, rawPayload, health sql.NullString
		var fill, weight, utilization, ogi, accumulation, temperature sql.NullFloat64
		var fisScore sql.NullFloat64
		var fisStatus, fisVersion, reason, safety sql.NullString
		if err := rows.Scan(&item.ReadingID, &item.RecordedAt, &item.LocationID, &item.LocationName,
			&item.DeviceID, &item.DeviceCode, &placement, &item.SlotID, &item.Stream,
			&rawPayload, &fill, &weight, &utilization, &ogi, &accumulation, &temperature, &health,
			&fisScore, &fisStatus, &fisVersion, &reason, &safety); err != nil {
			return nil, err
		}
		item.PlacementLabel = nullableString(placement)
		item.Fill = nullableFloat(fill)
		item.Weight = nullableFloat(weight)
		item.WeightUtilization = nullableFloat(utilization)
		item.GasRaw = rawGas(rawPayload.String, item.Stream)
		item.OGI = nullableFloat(ogi)
		item.AccumulationHours = nullableFloat(accumulation)
		item.Temperature = nullableFloat(temperature)
		item.FISScore = nullableFloat(fisScore)
		item.FISStatus = nullableString(fisStatus)
		item.FISVersion = nullableString(fisVersion)
		item.DominantReason = nullableString(reason)
		item.SafetyState = nullableString(safety)
		if health.Valid && json.Valid([]byte(health.String)) {
			item.SensorHealth = json.RawMessage(health.String)
		} else {
			item.SensorHealth = json.RawMessage("null")
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return items, nil
}

var csvHeader = []string{
	"recorded_at", "location_id", "location_name", "device_id", "device_code", "placement_label", "slot_id", "stream",
	"fill", "weight", "weight_utilization", "gas_raw", "ogi", "accumulation_hours", "temperature",
	"fis_score", "fis_status", "fis_version", "dominant_reason", "safety_state",
}

// WriteCSV writes the stable research export projection using encoding/csv so
// commas, quotes, and line breaks in names are escaped correctly.
func WriteCSV(output io.Writer, items []Record) error {
	writer := csv.NewWriter(output)
	if err := writer.Write(csvHeader); err != nil {
		return err
	}
	for _, item := range items {
		row := []string{
			item.RecordedAt, item.LocationID, item.LocationName, item.DeviceID, item.DeviceCode, valueString(item.PlacementLabel), item.SlotID, item.Stream,
			floatString(item.Fill), floatString(item.Weight), floatString(item.WeightUtilization), floatString(item.GasRaw), floatString(item.OGI), floatString(item.AccumulationHours), floatString(item.Temperature),
			floatString(item.FISScore), valueString(item.FISStatus), valueString(item.FISVersion), valueString(item.DominantReason), valueString(item.SafetyState),
		}
		if err := writer.Write(row); err != nil {
			return err
		}
	}
	writer.Flush()
	return writer.Error()
}

func rawGas(payload, stream string) *float64 {
	var envelope struct {
		Slots []struct {
			Stream string   `json:"stream"`
			GasRaw *float64 `json:"gas_raw"`
		} `json:"slots"`
	}
	if json.Unmarshal([]byte(payload), &envelope) != nil {
		return nil
	}
	for _, slot := range envelope.Slots {
		if slot.Stream == stream {
			return slot.GasRaw
		}
	}
	return nil
}

func isPublicStatus(value string) bool {
	switch device.Status(value) {
	case device.StatusNormal, device.StatusWaspada, device.StatusAngkat, device.StatusSafetyAlert, device.StatusDataInvalid:
		return true
	default:
		return false
	}
}

func nullableString(value sql.NullString) *string {
	if !value.Valid {
		return nil
	}
	return stringPointer(value.String)
}

func nullableFloat(value sql.NullFloat64) *float64 {
	if !value.Valid {
		return nil
	}
	result := value.Float64
	return &result
}

func valueString(value *string) string {
	if value == nil {
		return ""
	}
	return *value
}

func floatString(value *float64) string {
	if value == nil {
		return ""
	}
	return strconv.FormatFloat(*value, 'f', -1, 64)
}

func stringPointer(value string) *string { return &value }
