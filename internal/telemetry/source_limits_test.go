package telemetry

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

func TestProfileReferencesUseIndependentPerRecordBound(t *testing.T) {
	ref := `{"kind":"CALIBRATION","target":"hx711_organic","schema_version":1,"version":"1","hash":"sha256:` + strings.Repeat("0", 64) + `"}`
	refs := make([]string, MaxProfileRefsPerRecord+1)
	for index := range refs {
		refs[index] = ref
	}
	raw := json.RawMessage("[" + strings.Join(refs, ",") + "]")
	if code := errorCode(t, func() error {
		_, err := validateProfileRefs(raw)
		return err
	}()); code != ProfileMalformed {
		t.Fatalf("profile reference overflow code = %s", code)
	}
}

func TestResultSourceSampleIDsDoNotAliasBatchRecordCap(t *testing.T) {
	if MaxRecords != 128 {
		t.Fatalf("test assumes the frozen top-level batch cap, got %d", MaxRecords)
	}
	ids := make([]string, MaxRecords+1)
	for index := range ids {
		ids[index] = fmt.Sprintf("SIGAP-01/boot-A/%d", index+1)
	}
	encoded, err := json.Marshal(ids)
	if err != nil {
		t.Fatal(err)
	}
	var decoded []string
	if err := json.Unmarshal(encoded, &decoded); err != nil {
		t.Fatal(err)
	}
	if len(decoded) != MaxRecords+1 {
		t.Fatalf("source IDs were truncated: got %d", len(decoded))
	}
	// There is intentionally no source-ID count check. The enclosing record
	// byte limit remains the authoritative bounded-memory guard.
}

func TestResultAcceptsSourceIDsAboveBatchCapWithinRecordBudget(t *testing.T) {
	var batch map[string]any
	if err := json.Unmarshal(fixture(t, "fixtures/valid/result-multi-source.json"), &batch); err != nil {
		t.Fatal(err)
	}
	results := batch["results"].([]any)
	record := results[0].(map[string]any)
	ids := make([]string, MaxRecords+1)
	for index := range ids {
		ids[index] = fmt.Sprintf("SIGAP-01/boot-A/%d", index+1)
	}
	record["source_sample_ids"] = ids
	hashRecordForTest(t, record)
	batch["results"] = results
	body, err := json.Marshal(batch)
	if err != nil {
		t.Fatal(err)
	}
	if len(body) >= MaxRecordBytes {
		t.Fatalf("source ID fixture unexpectedly exceeds record budget: %d", len(body))
	}
	if _, err := ValidateBatch(body); err != nil {
		t.Fatalf("source IDs within record budget must remain intact: %v", err)
	}
}

func TestResultStreamsExcludeAmbient(t *testing.T) {
	if knownResultStreams["ambient"] {
		t.Fatal("ambient must not be a result stream")
	}
	for _, stream := range []string{"organic", "inorganic", "b3"} {
		if !knownResultStreams[stream] {
			t.Fatalf("result stream %q missing", stream)
		}
	}
}
