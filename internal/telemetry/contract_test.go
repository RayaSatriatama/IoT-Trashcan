package telemetry

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"testing"
)

func fixture(t *testing.T, name string) []byte {
	t.Helper()
	path := "../../contracts/telemetry/v3/" + name
	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read fixture %s: %v", path, err)
	}
	return contents
}

func errorCode(t *testing.T, err error) ErrorCode {
	t.Helper()
	if err == nil {
		t.Fatal("expected error")
	}
	var contractErr *ContractError
	if !errors.As(err, &contractErr) {
		t.Fatalf("expected ContractError, got %T: %v", err, err)
	}
	return contractErr.Code
}

func batchErrorCode(t *testing.T, body []byte) ErrorCode {
	t.Helper()
	_, err := ValidateBatch(body)
	return errorCode(t, err)
}

func TestUint64DecimalAndSortKey(t *testing.T) {
	for _, value := range []string{"0", "1", "18446744073709551615"} {
		if _, err := ParseUint64Decimal(value); err != nil {
			t.Fatalf("parse %s: %v", value, err)
		}
	}
	if got, err := SequenceSortKey("18446744073709551615"); err != nil || got != "18446744073709551615" {
		t.Fatalf("max sort key = %q, %v", got, err)
	}
	for _, value := range []string{"", "00", "01", "-1", "+1", "18446744073709551616", "1.0", " 1"} {
		if _, err := ParseUint64Decimal(value); errorCode(t, err) != InvalidUint64 {
			t.Fatalf("expected INVALID_UINT64 for %q", value)
		}
	}
}

func TestCanonicalRecordHashFixture(t *testing.T) {
	canonical := bytes.TrimSpace(fixture(t, "canonical/sample-mq4-uncalibrated.json"))
	expectedHash := strings.TrimSpace(string(fixture(t, "canonical/sample-mq4-uncalibrated.record-hash")))
	if string(canonical) == "" || bytes.Contains(canonical, []byte("record_hash")) {
		t.Fatal("canonical fixture must omit record_hash")
	}
	var object map[string]json.RawMessage
	if err := json.Unmarshal(canonical, &object); err != nil {
		t.Fatal(err)
	}
	object["record_hash"], _ = json.Marshal(expectedHash)
	record, _ := json.Marshal(object)
	got, canonicalBytes, err := RecordHash(record)
	if err != nil {
		t.Fatal(err)
	}
	if got != expectedHash {
		t.Fatalf("record hash = %s, want %s", got, expectedHash)
	}
	if !bytes.Equal(canonicalBytes, canonical) {
		t.Fatalf("canonical bytes differ\n got: %s\nwant: %s", canonicalBytes, canonical)
	}
}

func TestBodyHashUsesExactBytes(t *testing.T) {
	body := fixture(t, "body/body-minimal.json")
	pretty := fixture(t, "body/body-minimal-pretty.json")
	expected := strings.TrimSpace(string(fixture(t, "body/body-minimal.sha256")))
	if got := BodyHash(body); got != expected {
		t.Fatalf("body hash = %s, want %s", got, expected)
	}
	if BodyHash(body) == BodyHash(pretty) {
		t.Fatal("whitespace-only body change must change exact body hash")
	}
	if _, err := ValidateBatch(body); err != nil {
		t.Fatalf("minimal body should validate: %v", err)
	}
}

func TestValidFixtureCorpus(t *testing.T) {
	entries, err := os.ReadDir("../../contracts/telemetry/v3/fixtures/valid")
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) < 16 {
		t.Fatalf("valid fixture corpus = %d, want at least 16", len(entries))
	}
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		t.Run(entry.Name(), func(t *testing.T) {
			body, err := os.ReadFile("../../contracts/telemetry/v3/fixtures/valid/" + entry.Name())
			if err != nil {
				t.Fatal(err)
			}
			if _, err := ValidateBatch(body); err != nil {
				t.Fatalf("valid fixture rejected: %v", err)
			}
		})
	}
}

func TestPendingBatchFreezesExactRetryBytes(t *testing.T) {
	body := fixture(t, "body/body-minimal.json")
	pending, err := NewPendingBatch("SIGAP-01/boot-A/batch-1", body, 1)
	if err != nil {
		t.Fatal(err)
	}
	if pending.Attempted() {
		t.Fatal("batch must be mutable only before first send")
	}
	if err := pending.FreezeForFirstSend(); err != nil {
		t.Fatal(err)
	}
	frozenHash := pending.BodyHash()
	copyBody := pending.Body()
	copyBody[0] = '{'
	if pending.BodyHash() != frozenHash || !pending.BodyEquals(body) {
		t.Fatal("caller mutation changed frozen batch")
	}
	if err := pending.FreezeForFirstSend(); err != nil || !bytes.Equal(pending.RetryBody(), body) {
		t.Fatal("retry must use exact original bytes")
	}
}

func TestReplayAndRecordConflictChecksAreWholeBatchPreflight(t *testing.T) {
	body := fixture(t, "body/body-minimal.json")
	batch, err := ValidateBatch(body)
	if err != nil {
		t.Fatal(err)
	}
	if decision, err := CompareBatchReplay("sha256:abc", "sha256:abc"); err != nil || decision != ReplayIdentical {
		t.Fatalf("identical replay = %s, %v", decision, err)
	}
	if code := errorCode(t, func() error { _, err := CompareBatchReplay("sha256:abc", "sha256:def"); return err }()); code != Conflict {
		t.Fatalf("batch conflict code = %s", code)
	}
	existing := map[string]string{batch.Samples[0].RecordID: "sha256:" + strings.Repeat("f", 64)}
	if code := errorCode(t, CheckRecordConflicts(existing, batch)); code != Conflict {
		t.Fatalf("record conflict code = %s", code)
	}
}

func hashRecordForTest(t *testing.T, record map[string]any) []byte {
	t.Helper()
	withoutHash, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(withoutHash), `"record_hash"`) {
		record["record_hash"] = "sha256:" + strings.Repeat("0", 64)
		withoutHash, err = json.Marshal(record)
		if err != nil {
			t.Fatal(err)
		}
	}
	hash, _, err := RecordHash(withoutHash)
	if err != nil {
		t.Fatal(err)
	}
	record["record_hash"] = hash
	encoded, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}

func TestResultRequiresSourcesAndUnavailableModels(t *testing.T) {
	record := map[string]any{
		"record_id": "SIGAP-01/boot-A/9", "kind": "result", "device_id": "SIGAP-01", "boot_id": "boot-A", "sequence": "9", "monotonic_us": "900", "recorded_at": nil, "time_quality": "UNSYNCED",
		"config_ref": map[string]any{"schema_version": 1, "config_version": "4", "config_hash": "sha256:" + strings.Repeat("0", 64)}, "profile_refs": []any{}, "stream": "organic", "computed_monotonic_us": "900", "source_sample_ids": []string{}, "processed": map[string]any{},
		"fuzzy":  map[string]any{"state": "UNAVAILABLE", "score": nil, "reason": "INPUT_MISSING", "model_state": "UNCONFIGURED", "trace": map[string]any{}},
		"safety": map[string]any{"state": "UNAVAILABLE", "score": nil, "reason": "INPUT_MISSING", "model_state": "UNCONFIGURED", "trace": map[string]any{}},
	}
	recordBytes := hashRecordForTest(t, record)
	batch := `{"schema_version":3,"device_id":"SIGAP-01","batch_id":"SIGAP-01/boot-A/batch-result","record_count":1,"samples":[],"results":[` + string(recordBytes) + `],"events":[],"metadata":[]}`
	if code := batchErrorCode(t, []byte(batch)); code != SourceIDsMissing {
		t.Fatalf("missing source IDs code = %s", code)
	}
	record["source_sample_ids"] = []string{"SIGAP-01/boot-A/1"}
	recordBytes = hashRecordForTest(t, record)
	batch = `{"schema_version":3,"device_id":"SIGAP-01","batch_id":"SIGAP-01/boot-A/batch-result","record_count":1,"samples":[],"results":[` + string(recordBytes) + `],"events":[],"metadata":[]}`
	if _, err := ValidateBatch([]byte(batch)); err != nil {
		t.Fatalf("unavailable result should be structurally valid: %v", err)
	}
}

func TestValidateBatchRejectsDuplicateKeysAndUnknownFields(t *testing.T) {
	duplicate := []byte(`{"schema_version":3,"schema_version":3}`)
	if code := batchErrorCode(t, duplicate); code != DuplicateKey {
		t.Fatalf("duplicate key code = %s", code)
	}
	body := string(fixture(t, "body/body-minimal.json"))
	body = strings.Replace(body, `"metadata":[]}`, `"metadata":[],"unexpected":true}`, 1)
	if code := batchErrorCode(t, []byte(body)); code != UnknownField {
		t.Fatalf("unknown field code = %s", code)
	}
}

func TestValidateBatchRejectsUTF8LimitsAndSchema(t *testing.T) {
	if code := batchErrorCode(t, []byte{0xff}); code != InvalidUTF8 {
		t.Fatalf("invalid UTF-8 code = %s", code)
	}
	if code := batchErrorCode(t, []byte(`{"schema_version":2,"device_id":"SIGAP-01","batch_id":"SIGAP-01/b","record_count":0,"samples":[],"results":[],"events":[],"metadata":[]}`)); code != SchemaUnsupported {
		t.Fatalf("schema code = %s", code)
	}
	long := `{"schema_version":3,"device_id":"SIGAP-01","batch_id":"SIGAP-01/b","record_count":0,"samples":[],"results":[],"events":[],"metadata":[]}` + strings.Repeat(" ", MaxBodyBytes)
	if code := batchErrorCode(t, []byte(long)); code != BodyTooLarge {
		t.Fatalf("body bound code = %s", code)
	}
	deep := `{"schema_version":3,"device_id":"SIGAP-01","batch_id":"SIGAP-01/b","record_count":0,"samples":[],"results":[],"events":[],"metadata":[{"nested":{"nested":{"nested":{"nested":{"nested":{"nested":{"nested":{"nested":{"nested":{"nested":{"nested":{"nested":true}}}}}}}}}}}}]}`
	if code := batchErrorCode(t, []byte(deep)); code != DepthExceeded {
		t.Fatalf("depth bound code = %s", code)
	}
	longString := `{"schema_version":3,"device_id":"SIGAP-01","batch_id":"SIGAP-01/b","record_count":0,"samples":[],"results":[],"events":[],"metadata":[],"note":"` + strings.Repeat("x", MaxFreeStringBytes+1) + `"}`
	if code := batchErrorCode(t, []byte(longString)); code != StringTooLong {
		t.Fatalf("string bound code = %s", code)
	}
	nonFinite := `{"schema_version":3,"device_id":"SIGAP-01","batch_id":"SIGAP-01/b","record_count":0,"samples":[],"results":[],"events":[],"metadata":[],"value":NaN}`
	if code := batchErrorCode(t, []byte(nonFinite)); code != UnknownField && code != NonFiniteNumber && code != MalformedJSON {
		t.Fatalf("non-finite code = %s", code)
	}
}

func TestValidateBatchRejectsIdentityHashCountAndProfileErrors(t *testing.T) {
	body := string(fixture(t, "body/body-minimal.json"))
	wrongHash := strings.Replace(body, "sha256:a3a2593618943cc7a1d5f8bc0c90216dc22eb4c3297fb874887c11b7b6ef6883", "sha256:"+strings.Repeat("1", 64), 1)
	if code := batchErrorCode(t, []byte(wrongHash)); code != HashMismatch {
		t.Fatalf("wrong hash code = %s", code)
	}
	wrongID := strings.Replace(body, "SIGAP-01/boot-A/1", "SIGAP-01/boot-A/2", 1)
	if code := batchErrorCode(t, []byte(wrongID)); code != InvalidIdentity {
		t.Fatalf("wrong identity code = %s", code)
	}
	wrongCount := strings.Replace(body, `"record_count":1`, `"record_count":2`, 1)
	if code := batchErrorCode(t, []byte(wrongCount)); code != CountMismatch {
		t.Fatalf("wrong count code = %s", code)
	}
	badProfile := strings.Replace(body, `"profile_refs":[]`, `"profile_refs":[{"kind":"CALIBRATION","target":"hx711_organic","schema_version":1,"version":"1","hash":"bad"}]`, 1)
	if code := batchErrorCode(t, []byte(badProfile)); code != HashMismatch {
		t.Fatalf("bad profile code = %s", code)
	}
}

func TestValidateAckIdentityAndCounts(t *testing.T) {
	ack := []byte(`{"device_id":"SIGAP-01","batch_id":"SIGAP-01/boot-A/batch-1","body_hash":"sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c","record_count":2,"stored_count":1,"deduplicated_count":1,"status":"DURABLE"}`)
	want := AckExpectation{DeviceID: "SIGAP-01", BatchID: "SIGAP-01/boot-A/batch-1", BodyHash: "sha256:3620edf0548b8a15eb1638fc7d239b1995372a28982f7f1e194949160f166c7c", RecordCount: 2}
	if _, err := ValidateAck(ack, want); err != nil {
		t.Fatal(err)
	}
	bad := strings.Replace(string(ack), `"stored_count":1`, `"stored_count":0`, 1)
	_, badErr := ValidateAck([]byte(bad), want)
	if code := errorCode(t, badErr); code != AckMismatch {
		t.Fatalf("ACK code = %s", code)
	}
}
