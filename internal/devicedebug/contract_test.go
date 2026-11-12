package devicedebug

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const good = `{"schema_version":1,"device_id":"TEST-DEVICE","boot_id":"00000000000000000000000000000001","build_id":"TEST","level":"INFO","captured_monotonic_us":"9007199254740993","oldest_sequence":"1","overwritten":"0","delivery_lost":"0","events":[{"sequence":"1","monotonic_us":"9007199254740993","level":"INFO","module":1,"event_code":1,"request_id":null,"duration_us":0,"arg0":1,"arg1":0,"related_sequence":null}]}`

func TestDebugContractAcceptsCanonicalUpload(t *testing.T) {
	u, err := DecodeUpload([]byte(good))
	if err != nil {
		t.Fatal(err)
	}
	if u.Events[0].MonotonicUS != "9007199254740993" {
		t.Fatalf("uint64 rounded: %s", u.Events[0].MonotonicUS)
	}
	if _, err := DecodeUpload([]byte(" \n" + good + "	\n")); err != nil {
		t.Fatalf("rejected legal surrounding JSON whitespace: %v", err)
	}
}

func TestDebugContractAcceptsActualCppUpload(t *testing.T) {
	file := fixture(t, "http.json")
	if len(file) == 0 || file[len(file)-1] != '\n' || (len(file) > 1 && file[len(file)-2] == '\n') {
		t.Fatal("http.json must have exactly one storage LF")
	}
	fileDigest := sha256.Sum256(file)
	if hex.EncodeToString(fileDigest[:]) != "131751a4ef269c275005aaf9581daa668c1de585a683db39c2a8b5f889bec2f6" {
		t.Fatalf("unexpected C++ http.json file hash: %x", fileDigest)
	}
	wire := file[:len(file)-1]
	digest := sha256.Sum256(wire)
	if len(wire) != 406 || hex.EncodeToString(digest[:]) != "a48ea71d62b6f4a52bd63ce5e7c488d7a07f31e9ee15a98982161d4e1b98e5a3" {
		t.Fatalf("unexpected C++ wire size/hash: %d %x", len(wire), digest)
	}
	u, err := DecodeUpload(wire)
	if err != nil {
		t.Fatalf("actual C++ upload rejected: %v", err)
	}
	if u.DeviceID != "TEST-DEVICE" || u.BootID != "00000000000000000000000000000001" || len(u.Events) != 1 {
		t.Fatalf("unexpected producer envelope: %+v", u)
	}
	eventsFile := fixture(t, "events.json")
	if len(eventsFile) == 0 || eventsFile[len(eventsFile)-1] != '\n' {
		t.Fatal("events.json missing storage LF")
	}
	var event DebugEvent
	if err := json.Unmarshal(eventsFile[:len(eventsFile)-1], &event); err != nil {
		t.Fatal(err)
	}
	if event != u.Events[0] {
		t.Fatalf("event fixture mismatch: %+v != %+v", event, u.Events[0])
	}
	if u.Events[0].MonotonicUS != "9007199254740993" || u.Events[0].Sequence != "2" || u.Events[0].RequestID != nil || u.Events[0].RelatedSequence != nil {
		t.Fatalf("producer uint64/null semantics changed: %+v", u.Events[0])
	}
}

func TestDebugContractAcceptsSyntheticBLEObservationFromCppCodec(t *testing.T) {
	file := fixture(t, "ble-observation.txt")
	if len(file) == 0 || file[len(file)-1] != '\n' || (len(file) > 1 && file[len(file)-2] == '\n') {
		t.Fatal("ble-observation.txt must have exactly one storage LF")
	}
	var httpBody []byte
	labels := map[string]bool{}
	for _, line := range strings.Split(string(file[:len(file)-1]), "\n") {
		label, value, ok := strings.Cut(line, "=")
		if !ok || labels[label] {
			t.Fatalf("invalid/duplicate fixture line %q", line)
		}
		labels[label] = true
		if label == "HTTP_BLE_OBSERVATION" {
			httpBody = []byte(value)
		}
	}
	if len(labels) != 5 || len(httpBody) == 0 {
		t.Fatalf("incomplete actual-codec fixture labels: %v", labels)
	}
	u, err := DecodeUpload(httpBody)
	if err != nil {
		t.Fatalf("actual C++ BLE observation upload rejected: %v", err)
	}
	if len(u.Events) != 4 {
		t.Fatalf("event count=%d, want 4", len(u.Events))
	}
	wantCodes := []uint16{10, 10, 11, 11}
	wantArgs := [][2]int32{{0, 3}, {1, 3}, {3, 0}, {3, 1}}
	for i, event := range u.Events {
		if event.Module != 2 || event.EventCode != wantCodes[i] ||
			event.Arg0 != wantArgs[i][0] || event.Arg1 != wantArgs[i][1] {
			t.Fatalf("event %d lost BLE reason/count contract: %+v", i, event)
		}
	}
	if u.Events[0].RequestID == nil || *u.Events[0].RequestID != 0 || u.Events[1].RequestID != nil {
		t.Fatalf("request ID zero/absence changed: %+v %+v", u.Events[0], u.Events[1])
	}
	if u.Events[2].RelatedSequence == nil || *u.Events[2].RelatedSequence != "0" || u.Events[3].RelatedSequence != nil {
		t.Fatalf("related sequence zero/absence changed: %+v %+v", u.Events[2], u.Events[3])
	}
}

func TestDebugContractRejectsMissingNullAndUnknownFields(t *testing.T) {
	fields := []string{"schema_version", "device_id", "boot_id", "build_id", "level", "captured_monotonic_us", "oldest_sequence", "overwritten", "delivery_lost", "events"}
	for _, field := range fields {
		t.Run("missing_"+field, func(t *testing.T) {
			raw := strings.Replace(good, `"`+field+`":`, `"removed_`+field+`":`, 1)
			if _, err := DecodeUpload([]byte(raw)); err == nil {
				t.Fatalf("accepted missing %s", field)
			}
		})
	}
	for _, field := range []string{"sequence", "monotonic_us", "level", "module", "event_code", "request_id", "duration_us", "arg0", "arg1", "related_sequence"} {
		t.Run("missing_event_"+field, func(t *testing.T) {
			raw := strings.Replace(good, `"`+field+`":`, `"removed_`+field+`":`, 1)
			if _, err := DecodeUpload([]byte(raw)); err == nil {
				t.Fatalf("accepted missing event field %s", field)
			}
		})
	}
	for _, tc := range []struct{ name, raw string }{
		{"device_id_null", strings.Replace(good, `"device_id":"TEST-DEVICE"`, `"device_id":null`, 1)},
		{"build_id_null", strings.Replace(good, `"build_id":"TEST"`, `"build_id":null`, 1)},
		{"duration_null", strings.Replace(good, `"duration_us":0`, `"duration_us":null`, 1)},
		{"arg1_null", strings.Replace(good, `"arg1":0`, `"arg1":null`, 1)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := DecodeUpload([]byte(tc.raw)); err == nil {
				t.Fatal("accepted required null")
			}
		})
	}
	cases := []string{
		strings.Replace(good, `"events":[`, `"extra":1,"events":[`, 1),
		strings.Replace(good, `"arg0":1`, `"arg0":1,"extra":0`, 1),
		strings.Replace(good, `"schema_version":1,`, `"schema_version":1,"schema_version":1,`, 1),
		strings.Replace(good, `"schema_version":1,`, `"schema_version":1,"\u0073chema_version":1,`, 1),
		strings.Replace(good, `"sequence":"1"`, `"sequence":"01"`, 1),
	}
	for _, raw := range cases {
		if _, err := DecodeUpload([]byte(raw)); err == nil {
			t.Errorf("accepted invalid upload: %s", raw)
		}
	}
}

func TestDebugContractRejectsInvalidTypesAndCanonicalNumbers(t *testing.T) {
	cases := []struct{ name, raw string }{
		{"schema_null", strings.Replace(good, `"schema_version":1`, `"schema_version":null`, 1)},
		{"schema_fraction", strings.Replace(good, `"schema_version":1`, `"schema_version":1.0`, 1)},
		{"schema_exponent", strings.Replace(good, `"schema_version":1`, `"schema_version":1e0`, 1)},
		{"schema_negative_zero", strings.Replace(good, `"schema_version":1`, `"schema_version":-0`, 1)},
		{"module_null", strings.Replace(good, `"module":1`, `"module":null`, 1)},
		{"duration_negative", strings.Replace(good, `"duration_us":0`, `"duration_us":-1`, 1)},
		{"arg_fraction", strings.Replace(good, `"arg0":1`, `"arg0":1.5`, 1)},
		{"arg_overflow", strings.Replace(good, `"arg0":1`, `"arg0":2147483648`, 1)},
		{"arg_underflow", strings.Replace(good, `"arg1":0`, `"arg1":-2147483649`, 1)},
		{"request_overflow", strings.Replace(good, `"request_id":null`, `"request_id":4294967296`, 1)},
		{"duration_overflow", strings.Replace(good, `"duration_us":0`, `"duration_us":4294967296`, 1)},
		{"related_leading_zero", strings.Replace(good, `"related_sequence":null`, `"related_sequence":"00"`, 1)},
		{"seq_zero", strings.Replace(good, `"sequence":"1"`, `"sequence":"0"`, 1)},
		{"seq_overflow", strings.Replace(good, `"sequence":"1"`, `"sequence":"18446744073709551616"`, 1)},
		{"captured_negative", strings.Replace(good, `"captured_monotonic_us":"9007199254740993"`, `"captured_monotonic_us":"-1"`, 1)},
		{"unknown_zero_code", strings.Replace(good, `"module":1,"event_code":1`, `"module":0,"event_code":0`, 1)},
		{"unknown_code_zero_module", strings.Replace(good, `"module":1,"event_code":1`, `"module":0,"event_code":63`, 1)},
		{"known_code_wrong_module", strings.Replace(good, `"module":1,"event_code":1`, `"module":2,"event_code":1`, 1)},
		{"noncanonical_boot", strings.Replace(good, "00000000000000000000000000000001", "0000000000000000000000000000000A", 1)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := DecodeUpload([]byte(tc.raw)); err == nil {
				t.Fatal("accepted invalid upload")
			}
		})
	}
	optionalZero := strings.Replace(good, `"request_id":null`, `"request_id":0`, 1)
	optionalZero = strings.Replace(optionalZero, `"related_sequence":null`, `"related_sequence":"0"`, 1)
	zeroUpload, err := DecodeUpload([]byte(optionalZero))
	if err != nil {
		t.Fatalf("valid optional zero rejected: %v", err)
	}
	if zeroUpload.Events[0].RequestID == nil || *zeroUpload.Events[0].RequestID != 0 || zeroUpload.Events[0].RelatedSequence == nil || *zeroUpload.Events[0].RelatedSequence != "0" {
		t.Fatalf("optional zero lost: %+v", zeroUpload.Events[0])
	}
}

func TestDebugContractRejectsMalformedDocumentsAndUnicode(t *testing.T) {
	deep := `{"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h":{"i":0}}}}}}}}}`
	cases := [][]byte{[]byte(good + ` {}`), []byte(`{`), []byte(good[:len(good)-1]), []byte(strings.Replace(good, `"TEST-DEVICE"`, `"\ud800"`, 1)), []byte(strings.Replace(good, `"TEST-DEVICE"`, `"\ud800x"`, 1)), []byte(deep), []byte{0xff}}
	for i, raw := range cases {
		if _, err := DecodeUpload(raw); err == nil {
			t.Errorf("accepted malformed case %d", i)
		}
	}
	if _, err := DecodeUpload(append([]byte(good), make([]byte, maxBody)...)); err == nil {
		t.Fatal("accepted oversized body")
	}
	if _, err := DecodeUpload([]byte(good + strings.Repeat(" ", maxBody-len(good)))); err != nil {
		t.Fatalf("rejected 4096-byte body: %v", err)
	}
	invalidUTF8 := []byte(good)
	invalidUTF8[strings.Index(good, "TEST-DEVICE")] = 0xff
	if _, err := DecodeUpload(invalidUTF8); err == nil {
		t.Fatal("accepted invalid UTF-8 within JSON string")
	}
	unicodeUpload := strings.Replace(good, `"TEST-DEVICE"`, `"device-\ud83d\ude80"`, 1)
	if _, err := DecodeUpload([]byte(unicodeUpload)); err != nil {
		t.Fatalf("valid surrogate pair rejected: %v", err)
	}
}

func TestDebugContractRejectsEventLimitsAndOrder(t *testing.T) {
	cases := []struct{ name, raw string }{
		{"empty_events", strings.Replace(good, `"events":[{"sequence":"1","monotonic_us":"9007199254740993","level":"INFO","module":1,"event_code":1,"request_id":null,"duration_us":0,"arg0":1,"arg1":0,"related_sequence":null}]`, `"events":[]`, 1)},

		{"event_after_snapshot", strings.Replace(good, `"monotonic_us":"9007199254740993"`, `"monotonic_us":"9007199254740994"`, 1)},
		{"single_event_oversize", strings.Replace(good, `{"sequence"`, `{"sequence"`+strings.Repeat(" ", 350), 1)},
		{"oversized_identity", strings.Replace(good, `"device_id":"TEST-DEVICE"`, `"device_id":"`+strings.Repeat("D", 65)+`"`, 1)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := DecodeUpload([]byte(tc.raw)); err == nil {
				t.Fatal("accepted invalid event/envelope")
			}
		})
	}
}

func TestDebugContractEventCountAndSerializedSizeBoundaries(t *testing.T) {
	marker := `"events":[`
	start := strings.Index(good, marker) + len(marker)
	end := strings.LastIndex(good, `]}`)
	event := good[start:end]
	prefix, suffix := good[:start], good[end:]
	for count := 1; count <= 9; count++ {
		batch := make([]string, count)
		for i := range batch {
			batch[i] = strings.Replace(event, `"sequence":"1"`, `"sequence":"`+string(rune('1'+i))+`"`, 1)
		}
		_, err := DecodeUpload([]byte(prefix + strings.Join(batch, ",") + suffix))
		if count <= 8 && err != nil {
			t.Fatalf("accepted event count %d rejected: %v", count, err)
		}
		if count == 9 && err == nil {
			t.Fatal("accepted nine events")
		}
	}
	first := strings.Replace(event, `"sequence":"1"`, `"sequence":"2"`, 1)
	second := strings.Replace(event, `"sequence":"1"`, `"sequence":"1"`, 1)
	if _, err := DecodeUpload([]byte(prefix + first + "," + second + suffix)); err == nil {
		t.Fatal("accepted non-increasing event sequence")
	}
	padding := 512 - len(event)
	boundary := strings.Replace(event, `{`, `{`+strings.Repeat(" ", padding), 1)
	if len(boundary) != 512 {
		t.Fatalf("test construction produced %d-byte event", len(boundary))
	}
	if _, err := DecodeUpload([]byte(prefix + boundary + suffix)); err != nil {
		t.Fatalf("rejected 512-byte event: %v", err)
	}
	oversize := strings.Replace(boundary, `{`, `{ `, 1)
	if _, err := DecodeUpload([]byte(prefix + oversize + suffix)); err == nil {
		t.Fatal("accepted 513-byte event")
	}
}

func TestDebugContractAcceptsIntegerAndIdentityMaxima(t *testing.T) {
	raw := strings.ReplaceAll(good, "9007199254740993", "18446744073709551615")
	raw = strings.Replace(raw, `"sequence":"1"`, `"sequence":"18446744073709551615"`, 1)
	raw = strings.Replace(raw, `"device_id":"TEST-DEVICE"`, `"device_id":"`+strings.Repeat("D", 64)+`"`, 1)
	raw = strings.Replace(raw, `"build_id":"TEST"`, `"build_id":"`+strings.Repeat("B", 96)+`"`, 1)
	raw = strings.Replace(raw, `"request_id":null`, `"request_id":4294967295`, 1)
	raw = strings.Replace(raw, `"duration_us":0`, `"duration_us":4294967295`, 1)
	raw = strings.Replace(raw, `"arg0":1`, `"arg0":2147483647`, 1)
	raw = strings.Replace(raw, `"arg1":0`, `"arg1":-2147483648`, 1)
	raw = strings.Replace(raw, `"related_sequence":null`, `"related_sequence":"18446744073709551615"`, 1)
	if _, err := DecodeUpload([]byte(raw)); err != nil {
		t.Fatalf("rejected valid maxima: %v", err)
	}
}

func TestDebugContractBuildsActualGoAckFixture(t *testing.T) {
	file := fixture(t, "http.json")
	wire := file[:len(file)-1]
	u, err := DecodeUpload(wire)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(wire)
	ack := UploadAck{SchemaVersion: 1, Status: "DURABLE", DeviceID: u.DeviceID, BootID: u.BootID, FirstSequence: u.Events[0].Sequence, LastSequence: u.Events[len(u.Events)-1].Sequence, StoredCount: uint8(len(u.Events)), DeduplicatedCount: 0, RequestSHA256: hex.EncodeToString(digest[:])}
	actual, err := json.Marshal(ack)
	if err != nil {
		t.Fatal(err)
	}
	stored := fixture(t, "ack.json")
	if !strings.HasSuffix(string(stored), "\n") || strings.HasSuffix(string(stored), "\n\n") {
		t.Fatal("ack.json must end with exactly one LF")
	}
	if string(stored[:len(stored)-1]) != string(actual) {
		t.Fatalf("ACK fixture is not json.Marshal(UploadAck): got %s want %s", stored, append(actual, '\n'))
	}
	var got UploadAck
	if err := json.Unmarshal(stored[:len(stored)-1], &got); err != nil {
		t.Fatal(err)
	}
	if got != ack {
		t.Fatalf("ACK does not bind exact producer upload: got %+v want %+v", got, ack)
	}
}

func fixture(t *testing.T, name string) []byte {
	t.Helper()
	path := filepath.Join("..", "..", "contracts", "debug", "v1", "fixtures", name)
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read fixture %s: %v", name, err)
	}
	return b
}
