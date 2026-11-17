package telemetryarchive

import (
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
)

const historyDefaultLimit = 200
const historyMaxLimit = 200

var historyIdentity = regexp.MustCompile(`^[A-Za-z0-9._-]{1,128}$`)
var historySensors = map[string]map[string]map[string]bool{
	"hx711_organic":        {"counts": {"counts": true}, "converted": {"kg": true}},
	"hx711_inorganic":      {"counts": {"counts": true}, "converted": {"kg": true}},
	"hx711_b3":             {"counts": {"counts": true}, "converted": {"kg": true}},
	"ultrasonic_organic":   {"counts": {"echo_us": true}, "converted": {"distance_mm": true}},
	"ultrasonic_inorganic": {"counts": {"echo_us": true}, "converted": {"distance_mm": true}},
	"ultrasonic_b3":        {"counts": {"echo_us": true}, "converted": {"distance_mm": true}},
	"mq4":                  {"counts": {"adc_counts": true}, "converted": {"adc_mv": true}},
	"mq135":                {"counts": {"adc_counts": true}, "converted": {"adc_mv": true, "adc_pin_mv": true, "sensor_ao_mv": true, "adc_efuse_calibrated": true}},
	"mq7":                  {"counts": {"adc_counts": true}, "converted": {"adc_mv": true}},
	"mq8":                  {"counts": {"adc_counts": true}, "converted": {"adc_mv": true}},
	"ds18b20_b3":           {"counts": {"temperature_raw": true}, "converted": {"temperature_c": true}},
	"sht31_44":             {"counts": {"temperature_ticks": true, "humidity_ticks": true}, "converted": {"temperature_c": true, "humidity_percent": true}},
	"sht31_45":             {"counts": {"temperature_ticks": true, "humidity_ticks": true}, "converted": {"temperature_c": true, "humidity_percent": true}},
}

type SensorHistoryPoint struct {
	BootID       string             `json:"boot_id"`
	Sequence     string             `json:"sequence"`
	MonotonicUS  string             `json:"monotonic_us"`
	RecordedAt   *string            `json:"recorded_at"`
	TimeQuality  string             `json:"time_quality"`
	Quality      string             `json:"quality"`
	Reason       string             `json:"reason"`
	SampleOrigin string             `json:"sample_origin"`
	Raw          map[string]float64 `json:"raw"`
	Converted    map[string]float64 `json:"converted"`
}
type SensorHistoryPage struct {
	DeviceID          string               `json:"device_id"`
	SensorID          string               `json:"sensor_id"`
	Points            []SensorHistoryPoint `json:"points"`
	NextCursor        *string              `json:"next_cursor,omitempty"`
	UnavailableReason *string              `json:"unavailable_reason,omitempty"`
}
type historyCursor struct {
	Version      int    `json:"v"`
	Device       string `json:"d"`
	Sensor       string `json:"s"`
	Boot         string `json:"b"`
	BoundaryMono string `json:"h"`
	BoundarySeq  string `json:"q"`
	BoundaryID   string `json:"i"`
	Mono         string `json:"m"`
	Sequence     string `json:"n"`
	RecordID     string `json:"r"`
	Snapshot     int64  `json:"x"`
}

func (q *ArchiveQuery) ListSensorHistory(ctx context.Context, deviceID, sensorID, bootID, cursor string, limit int) (SensorHistoryPage, error) {
	if q == nil || q.db == nil || !historyIdentity.MatchString(deviceID) || !historyIdentity.MatchString(sensorID) || historySensors[sensorID] == nil || limit < 0 || (bootID != "" && !historyIdentity.MatchString(bootID)) {
		return SensorHistoryPage{}, fmt.Errorf("%w: invalid history query", ErrInvalidQuery)
	}
	if limit == 0 {
		limit = historyDefaultLimit
	}
	if limit > historyMaxLimit {
		limit = historyMaxLimit
	}
	var c historyCursor
	if cursor != "" {
		if len(cursor) > 4096 {
			return SensorHistoryPage{}, fmt.Errorf("%w: cursor is too large", ErrInvalidQuery)
		}
		raw, err := base64.RawURLEncoding.DecodeString(cursor)
		if err != nil || len(raw) > 2048 {
			return SensorHistoryPage{}, fmt.Errorf("%w: malformed cursor", ErrInvalidQuery)
		}
		if json.Unmarshal(raw, &c) != nil || c.Version != 1 || c.Device != deviceID || c.Sensor != sensorID || c.Boot != bootID || !validDecimal(c.BoundaryMono) || !validDecimal(c.BoundarySeq) || !validDecimal(c.Mono) || !validDecimal(c.Sequence) || c.BoundaryID == "" || c.RecordID == "" || c.Snapshot < 1 {
			return SensorHistoryPage{}, fmt.Errorf("%w: cursor context mismatch", ErrInvalidQuery)
		}
	}
	if bootID == "" {
		return SensorHistoryPage{DeviceID: deviceID, SensorID: sensorID, Points: []SensorHistoryPoint{}, UnavailableReason: strptr("no comparable current boot")}, nil
	}
	snapshot := c.Snapshot
	if cursor == "" {
		if err := q.db.QueryRowContext(ctx, `SELECT COALESCE(MAX(rowid),0) FROM telemetry_records`).Scan(&snapshot); err != nil {
			return SensorHistoryPage{}, fmt.Errorf("read sensor history snapshot: %w", err)
		}
	}
	if snapshot == 0 {
		return SensorHistoryPage{DeviceID: deviceID, SensorID: sensorID, Points: []SensorHistoryPoint{}, UnavailableReason: strptr("sensor or boot unavailable")}, nil
	}
	var malformed int
	badRecord := `SELECT 1 FROM telemetry_records WHERE device_id=? AND kind='sample' AND sensor_id=? AND boot_id=? AND rowid<=? AND (length(monotonic_us)=0 OR length(monotonic_us)>20 OR monotonic_us GLOB '*[^0-9]*' OR (length(monotonic_us)>1 AND substr(monotonic_us,1,1)='0') OR length(sequence_decimal)=0 OR length(sequence_decimal)>20 OR sequence_decimal GLOB '*[^0-9]*' OR (length(sequence_decimal)>1 AND substr(sequence_decimal,1,1)='0') OR sequence_sort<>substr('00000000000000000000'||sequence_decimal,-20)) LIMIT 1`
	checkErr := q.db.QueryRowContext(ctx, badRecord, deviceID, sensorID, bootID, snapshot).Scan(&malformed)
	if checkErr == nil {
		return SensorHistoryPage{}, fmt.Errorf("malformed stored history identity")
	}
	if !errors.Is(checkErr, sql.ErrNoRows) {
		return SensorHistoryPage{}, fmt.Errorf("validate stored history identities: %w", checkErr)
	}
	where := `device_id=? AND kind='sample' AND sensor_id=? AND boot_id=? AND rowid<=? AND length(monotonic_us)<=20 AND monotonic_us NOT GLOB '*[^0-9]*'`
	args := []any{deviceID, sensorID, bootID, snapshot}
	if cursor != "" {
		where += ` AND (length(monotonic_us)<length(?) OR (length(monotonic_us)=length(?) AND (monotonic_us<? OR (monotonic_us=? AND (sequence_sort<? OR (sequence_sort=? AND record_id<=?)))))) AND (length(monotonic_us)<length(?) OR (length(monotonic_us)=length(?) AND monotonic_us<?) OR (monotonic_us=? AND (sequence_sort<? OR (sequence_sort=? AND record_id<?))))`
		boundaryKey := strings.Repeat("0", 20-len(c.BoundarySeq)) + c.BoundarySeq
		sortKey := strings.Repeat("0", 20-len(c.Sequence)) + c.Sequence
		args = append(args, c.BoundaryMono, c.BoundaryMono, c.BoundaryMono, c.BoundaryMono, boundaryKey, boundaryKey, c.BoundaryID, c.Mono, c.Mono, c.Mono, c.Mono, sortKey, sortKey, c.RecordID)
	}
	rows, err := q.db.QueryContext(ctx, `SELECT record_id,sequence_decimal,sequence_sort,monotonic_us,recorded_at,time_quality,record_bytes FROM telemetry_records WHERE `+where+` ORDER BY length(monotonic_us) DESC,monotonic_us DESC,sequence_sort DESC,record_id DESC LIMIT ?`, append(args, limit+1)...)
	if err != nil {
		return SensorHistoryPage{}, fmt.Errorf("query sensor history: %w", err)
	}
	defer rows.Close()
	type row struct {
		id, seq, sort, mono, timeQuality string
		at                               sql.NullString
		raw                              []byte
	}
	items := make([]row, 0, limit+1)
	for rows.Next() {
		var x row
		if err := rows.Scan(&x.id, &x.seq, &x.sort, &x.mono, &x.at, &x.timeQuality, &x.raw); err != nil {
			return SensorHistoryPage{}, fmt.Errorf("scan sensor history: %w", err)
		}
		if !validDecimal(x.seq) || x.sort != strings.Repeat("0", 20-len(x.seq))+x.seq || !validDecimal(x.mono) {
			return SensorHistoryPage{}, fmt.Errorf("malformed stored history ordering for record %q", x.id)
		}
		items = append(items, x)
	}
	if err := rows.Err(); err != nil {
		return SensorHistoryPage{}, err
	}
	if len(items) == 0 && cursor == "" {
		return SensorHistoryPage{DeviceID: deviceID, SensorID: sensorID, Points: []SensorHistoryPoint{}, UnavailableReason: strptr("sensor or boot unavailable")}, nil
	}
	page := SensorHistoryPage{DeviceID: deviceID, SensorID: sensorID, Points: make([]SensorHistoryPoint, 0, minInt(limit, len(items)))}
	hasMore := len(items) > limit
	if hasMore {
		items = items[:limit]
	}
	for _, x := range items {
		var rec struct {
			DeviceID  string                     `json:"device_id"`
			RecordID  string                     `json:"record_id"`
			SensorID  string                     `json:"sensor_id"`
			Boot      string                     `json:"boot_id"`
			Sequence  string                     `json:"sequence"`
			Monotonic string                     `json:"monotonic_us"`
			Quality   string                     `json:"quality"`
			Reason    string                     `json:"reason"`
			Raw       map[string]json.RawMessage `json:"raw"`
			Converted map[string]json.RawMessage `json:"converted"`
		}
		if err := json.Unmarshal(x.raw, &rec); err != nil {
			return SensorHistoryPage{}, fmt.Errorf("malformed stored sample %q: %w", x.id, err)
		}
		if rec.DeviceID != deviceID || rec.RecordID != x.id || rec.SensorID != sensorID || rec.Boot != bootID || rec.Sequence != x.seq || rec.Monotonic != x.mono {
			return SensorHistoryPage{}, fmt.Errorf("stored sample identity mismatch for record %q", x.id)
		}
		p := SensorHistoryPoint{BootID: rec.Boot, Sequence: x.seq, MonotonicUS: x.mono, TimeQuality: x.timeQuality, Quality: rec.Quality, Reason: rec.Reason, SampleOrigin: "UNKNOWN", Raw: historyNumbers(rec.Raw, historySensors[sensorID]["counts"]), Converted: historyNumbers(rec.Converted, historySensors[sensorID]["converted"])}
		if x.at.Valid {
			p.RecordedAt = &x.at.String
		}
		page.Points = append(page.Points, p)
	}
	if len(items) > 0 {
		origin := historyOrigin(ctx, q.db, deviceID, bootID)
		for i := range page.Points {
			page.Points[i].SampleOrigin = origin
		}
	}
	if hasMore {
		last := items[len(items)-1]
		boundary := items[0]
		bmono, bseq, bid := boundary.mono, boundary.seq, boundary.id
		if cursor != "" {
			bmono, bseq, bid = c.BoundaryMono, c.BoundarySeq, c.BoundaryID
		}
		raw, _ := json.Marshal(historyCursor{Version: 1, Device: deviceID, Sensor: sensorID, Boot: bootID, BoundaryMono: bmono, BoundarySeq: bseq, BoundaryID: bid, Mono: last.mono, Sequence: last.seq, RecordID: last.id, Snapshot: snapshot})
		token := base64.RawURLEncoding.EncodeToString(raw)
		page.NextCursor = &token
	}
	return page, nil
}
func validDecimal(s string) bool {
	if s == "" || (len(s) > 1 && s[0] == '0') {
		return false
	}
	_, err := strconv.ParseUint(s, 10, 64)
	return err == nil
}
func strptr(s string) *string { return &s }
func historyNumbers(fields map[string]json.RawMessage, allowed map[string]bool) map[string]float64 {
	var out map[string]float64
	for k, v := range fields {
		if !allowed[k] {
			continue
		}
		var n *float64
		if json.Unmarshal(v, &n) == nil && n != nil && !math.IsInf(*n, 0) && !math.IsNaN(*n) {
			if out == nil {
				out = map[string]float64{}
			}
			out[k] = *n
		}
	}
	return out
}
func historyOrigin(ctx context.Context, db *sql.DB, device, boot string) string {
	rows, err := db.QueryContext(ctx, `SELECT record_bytes FROM telemetry_records WHERE device_id=? AND boot_id=? AND kind='metadata' AND json_extract(CAST(record_bytes AS TEXT),'$.metadata_type')='BOOT' LIMIT 2`, device, boot)
	if err != nil {
		return "UNKNOWN"
	}
	defer rows.Close()
	var versions []string
	for rows.Next() {
		var raw []byte
		if rows.Scan(&raw) != nil {
			return "UNKNOWN"
		}
		var rec struct {
			MetadataType string `json:"metadata_type"`
			Payload      struct {
				FirmwareVersion string `json:"firmware_version"`
			} `json:"payload"`
		}
		if json.Unmarshal(raw, &rec) != nil || rec.MetadataType != "BOOT" || rec.Payload.FirmwareVersion == "" {
			return "UNKNOWN"
		}
		versions = append(versions, rec.Payload.FirmwareVersion)
	}
	if rows.Err() != nil || len(versions) != 1 || strings.TrimSpace(versions[0]) != versions[0] {
		return "UNKNOWN"
	}
	switch versions[0] {
	case "sigap-task6-sensor-sim-v1":
		return "SIMULATED"
	case "sigap-task6", "sigap-task6-physical4-v1", "sigap-task6-physical4-v2":
		return "PHYSICAL"
	}
	return "UNKNOWN"
}
