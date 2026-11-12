package devicedebug

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"strconv"
	"unicode/utf8"
)

const maxBody = 4096

var ErrSchema = errors.New("DEBUG_SCHEMA_INVALID")
var ErrRange = errors.New("DEBUG_RANGE_INVALID")
var ErrDuplicate = errors.New("DEBUG_DUPLICATE_KEY")

type DebugEvent struct {
	Sequence        string  `json:"sequence"`
	MonotonicUS     string  `json:"monotonic_us"`
	Level           string  `json:"level"`
	Module          uint8   `json:"module"`
	EventCode       uint16  `json:"event_code"`
	RequestID       *uint32 `json:"request_id"`
	DurationUS      uint32  `json:"duration_us"`
	Arg0            int32   `json:"arg0"`
	Arg1            int32   `json:"arg1"`
	RelatedSequence *string `json:"related_sequence"`
}
type Upload struct {
	SchemaVersion       uint8        `json:"schema_version"`
	DeviceID            string       `json:"device_id"`
	BootID              string       `json:"boot_id"`
	BuildID             string       `json:"build_id"`
	Level               string       `json:"level"`
	CapturedMonotonicUS string       `json:"captured_monotonic_us"`
	OldestSequence      string       `json:"oldest_sequence"`
	Overwritten         string       `json:"overwritten"`
	DeliveryLost        string       `json:"delivery_lost"`
	Events              []DebugEvent `json:"events"`
}

type UploadAck struct {
	SchemaVersion     uint8  `json:"schema_version"`
	Status            string `json:"status"`
	DeviceID          string `json:"device_id"`
	BootID            string `json:"boot_id"`
	FirstSequence     string `json:"first_sequence"`
	LastSequence      string `json:"last_sequence"`
	StoredCount       uint8  `json:"stored_count"`
	DeduplicatedCount uint8  `json:"deduplicated_count"`
	RequestSHA256     string `json:"request_sha256"`
}

type HistoryEvent struct {
	DebugEvent
	IngestionID         string `json:"ingestion_id"`
	ReceivedAt          string `json:"received_at"`
	BootID              string `json:"boot_id"`
	BuildID             string `json:"build_id"`
	SnapshotLevel       string `json:"snapshot_level"`
	CapturedMonotonicUS string `json:"captured_monotonic_us"`
	OldestSequence      string `json:"oldest_sequence"`
	Overwritten         string `json:"overwritten"`
	DeliveryLost        string `json:"delivery_lost"`
}

type HistoryPage struct {
	DeviceID        string         `json:"device_id"`
	Events          []HistoryEvent `json:"events"`
	NextAfter       string         `json:"next_after"`
	OldestAvailable string         `json:"oldest_available"`
	RetentionGap    bool           `json:"retention_gap"`
	HasMore         bool           `json:"has_more"`
}

var eventModule = map[uint16]uint8{1: 1, 2: 1, 3: 1, 10: 2, 11: 2, 12: 2, 13: 2, 14: 2, 15: 2, 16: 2, 20: 3, 21: 3, 22: 3, 23: 3, 24: 3, 25: 3, 30: 4, 31: 4, 32: 4, 40: 5, 41: 5, 42: 5, 50: 6, 51: 6, 52: 6, 53: 6, 60: 7, 61: 7, 62: 7}

func canonicalU64(s string, positive bool) bool {
	if s == "" || (len(s) > 1 && s[0] == '0') {
		return false
	}
	v, err := strconv.ParseUint(s, 10, 64)
	return err == nil && (!positive || v > 0)
}
func depthAndDuplicates(raw []byte) error {
	d := json.NewDecoder(bytes.NewReader(raw))
	d.UseNumber()
	var walk func(int) error
	walk = func(depth int) error {
		if depth > 8 {
			return ErrRange
		}
		t, err := d.Token()
		if err != nil {
			return ErrSchema
		}
		delim, ok := t.(json.Delim)
		if !ok {
			return nil
		}
		switch delim {
		case '{':
			seen := map[string]bool{}
			for d.More() {
				k, e := d.Token()
				if e != nil {
					return ErrSchema
				}
				key, ok := k.(string)
				if !ok {
					return ErrSchema
				}
				if seen[key] {
					return ErrDuplicate
				}
				seen[key] = true
				if e = walk(depth + 1); e != nil {
					return e
				}
			}
			_, err = d.Token()
			if err != nil {
				return ErrSchema
			}
		case '[':
			for d.More() {
				if err = walk(depth + 1); err != nil {
					return err
				}
			}
			_, err = d.Token()
			if err != nil {
				return ErrSchema
			}
		default:
			return ErrSchema
		}
		return nil
	}
	if err := walk(0); err != nil {
		return err
	}
	if _, err := d.Token(); err != io.EOF {
		return ErrSchema
	}
	return nil
}

func validUnicodeEscapes(raw []byte) bool {
	for i := 0; i < len(raw); {
		if raw[i] != '"' {
			i++
			continue
		}
		i++
		for i < len(raw) && raw[i] != '"' {
			if raw[i] != '\\' {
				i++
				continue
			}
			i++
			if i >= len(raw) {
				return false
			}
			if raw[i] != 'u' {
				i++
				continue
			}
			if i+4 >= len(raw) {
				return false
			}
			code, err := strconv.ParseUint(string(raw[i+1:i+5]), 16, 16)
			if err != nil {
				return false
			}
			i += 5
			if code >= 0xdc00 && code <= 0xdfff {
				return false
			}
			if code >= 0xd800 && code <= 0xdbff {
				if i+6 >= len(raw) || raw[i] != '\\' || raw[i+1] != 'u' {
					return false
				}
				low, err := strconv.ParseUint(string(raw[i+2:i+6]), 16, 16)
				if err != nil || low < 0xdc00 || low > 0xdfff {
					return false
				}
				i += 6
			}
		}
		if i >= len(raw) {
			return false
		}
		i++
	}
	return true
}

func object(raw json.RawMessage, keys ...string) (map[string]json.RawMessage, error) {
	raw = bytes.TrimSpace(raw)
	var fields map[string]json.RawMessage
	if len(raw) == 0 || raw[0] != '{' || json.Unmarshal(raw, &fields) != nil || fields == nil || len(fields) != len(keys) {
		return nil, ErrSchema
	}
	for _, key := range keys {
		if _, ok := fields[key]; !ok {
			return nil, ErrSchema
		}
	}
	return fields, nil
}

func stringField(fields map[string]json.RawMessage, key string) (string, error) {
	raw := fields[key]
	if len(raw) == 0 || raw[0] != '"' {
		return "", ErrSchema
	}
	var value string
	if json.Unmarshal(raw, &value) != nil {
		return "", ErrSchema
	}
	return value, nil
}

func integerField(fields map[string]json.RawMessage, key string, bits int, signed bool) (int64, error) {
	raw := string(fields[key])
	if raw == "" || raw == "-0" || (raw[0] == '-' && !signed) {
		return 0, ErrRange
	}
	digits := raw
	if signed && len(digits) > 0 && digits[0] == '-' {
		digits = digits[1:]
	}
	if digits == "" || (len(digits) > 1 && digits[0] == '0') {
		return 0, ErrRange
	}
	for _, c := range digits {
		if c < '0' || c > '9' {
			return 0, ErrRange
		}
	}
	if signed {
		value, err := strconv.ParseInt(raw, 10, bits)
		if err != nil {
			return 0, ErrRange
		}
		return value, nil
	}
	value, err := strconv.ParseUint(raw, 10, bits)
	if err != nil {
		return 0, ErrRange
	}
	return int64(value), nil
}

func decimalString(fields map[string]json.RawMessage, key string, positive bool) (uint64, error) {
	value, err := stringField(fields, key)
	if err != nil || !canonicalU64(value, positive) {
		return 0, ErrRange
	}
	parsed, err := strconv.ParseUint(value, 10, 64)
	if err != nil {
		return 0, ErrRange
	}
	return parsed, nil
}

func validateEvent(raw json.RawMessage) error {
	if len(raw) > 512 {
		return ErrRange
	}
	f, err := object(raw, "sequence", "monotonic_us", "level", "module", "event_code", "request_id", "duration_us", "arg0", "arg1", "related_sequence")
	if err != nil {
		return err
	}
	if _, err = decimalString(f, "sequence", true); err != nil {
		return err
	}
	if _, err = decimalString(f, "monotonic_us", false); err != nil {
		return err
	}
	level, err := stringField(f, "level")
	if err != nil || !validLevel(level) {
		return ErrSchema
	}
	module, err := integerField(f, "module", 8, false)
	if err != nil {
		return err
	}
	code, err := integerField(f, "event_code", 16, false)
	if err != nil {
		return err
	}
	if _, ok := eventModule[uint16(code)]; !ok || int64(eventModule[uint16(code)]) != module {
		return ErrRange
	}
	request := f["request_id"]
	if string(request) != "null" {
		if _, err = integerField(f, "request_id", 32, false); err != nil {
			return err
		}
	}
	if _, err = integerField(f, "duration_us", 32, false); err != nil {
		return err
	}
	if _, err = integerField(f, "arg0", 32, true); err != nil {
		return err
	}
	if _, err = integerField(f, "arg1", 32, true); err != nil {
		return err
	}
	related := f["related_sequence"]
	if string(related) != "null" {
		if _, err = decimalString(f, "related_sequence", false); err != nil {
			return err
		}
	}
	return nil
}

func validLevel(level string) bool {
	return level == "ERROR" || level == "WARN" || level == "INFO" || level == "DEBUG"
}

func DecodeUpload(raw []byte) (Upload, error) {
	var u Upload
	if len(raw) == 0 || len(raw) > maxBody || !utf8.Valid(raw) {
		return u, ErrRange
	}
	if !validUnicodeEscapes(raw) {
		return u, ErrSchema
	}
	if err := depthAndDuplicates(raw); err != nil {
		return u, err
	}
	fields, err := object(json.RawMessage(raw), "schema_version", "device_id", "boot_id", "build_id", "level", "captured_monotonic_us", "oldest_sequence", "overwritten", "delivery_lost", "events")
	if err != nil {
		return u, err
	}
	version, err := integerField(fields, "schema_version", 8, false)
	if err != nil || version != 1 {
		return u, ErrSchema
	}
	u.SchemaVersion = uint8(version)
	if u.DeviceID, err = stringField(fields, "device_id"); err != nil {
		return u, err
	}
	if u.BootID, err = stringField(fields, "boot_id"); err != nil {
		return u, err
	}
	if u.BuildID, err = stringField(fields, "build_id"); err != nil {
		return u, err
	}
	if u.Level, err = stringField(fields, "level"); err != nil {
		return u, err
	}
	if u.CapturedMonotonicUS, err = stringField(fields, "captured_monotonic_us"); err != nil {
		return u, err
	}
	if u.OldestSequence, err = stringField(fields, "oldest_sequence"); err != nil {
		return u, err
	}
	if u.Overwritten, err = stringField(fields, "overwritten"); err != nil {
		return u, err
	}
	if u.DeliveryLost, err = stringField(fields, "delivery_lost"); err != nil {
		return u, err
	}
	if u.DeviceID == "" || len([]byte(u.DeviceID)) > 64 || len(u.BootID) != 32 || u.BuildID == "" || len(u.BuildID) > 96 || !validLevel(u.Level) {
		return Upload{}, ErrSchema
	}
	for _, c := range u.BootID {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return Upload{}, ErrSchema
		}
	}
	for _, c := range u.BuildID {
		if c < 0x20 || c > 0x7e {
			return Upload{}, ErrSchema
		}
	}
	captured, err := decimalString(fields, "captured_monotonic_us", false)
	if err != nil {
		return Upload{}, err
	}
	if _, err = decimalString(fields, "oldest_sequence", false); err != nil {
		return Upload{}, err
	}
	if _, err = decimalString(fields, "overwritten", false); err != nil {
		return Upload{}, err
	}
	if _, err = decimalString(fields, "delivery_lost", false); err != nil {
		return Upload{}, err
	}
	var events []json.RawMessage
	if len(fields["events"]) == 0 || fields["events"][0] != '[' || json.Unmarshal(fields["events"], &events) != nil {
		return Upload{}, ErrSchema
	}
	if len(events) < 1 || len(events) > 8 {
		return Upload{}, ErrRange
	}
	u.Events = make([]DebugEvent, 0, len(events))
	var prev uint64
	for i, rawEvent := range events {
		if err := validateEvent(rawEvent); err != nil {
			return Upload{}, err
		}
		var e DebugEvent
		if err := json.Unmarshal(rawEvent, &e); err != nil {
			return Upload{}, ErrSchema
		}
		seq, _ := strconv.ParseUint(e.Sequence, 10, 64)
		mono, _ := strconv.ParseUint(e.MonotonicUS, 10, 64)
		if (i > 0 && seq <= prev) || mono > captured {
			return Upload{}, ErrRange
		}
		prev = seq
		u.Events = append(u.Events, e)
	}
	return u, nil
}
