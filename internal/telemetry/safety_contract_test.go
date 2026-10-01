package telemetry

import (
	"encoding/json"
	"testing"
)

// safetyFixture returns a result batch with one safety object replaced and its
// record hash recomputed. This keeps the assertions focused on the safety
// contract instead of hash bookkeeping.
func safetyFixture(t *testing.T, safety map[string]any) []byte {
	t.Helper()
	var batch map[string]any
	if err := json.Unmarshal(fixture(t, "fixtures/valid/result-safety-unavailable.json"), &batch); err != nil {
		t.Fatal(err)
	}
	results, ok := batch["results"].([]any)
	if !ok || len(results) != 1 {
		t.Fatal("safety fixture must contain one result")
	}
	record, ok := results[0].(map[string]any)
	if !ok {
		t.Fatal("safety fixture result must be an object")
	}
	record["safety"] = safety
	delete(record, "record_hash")
	results[0] = json.RawMessage(hashRecordForTest(t, record))
	batch["results"] = results
	body, err := json.Marshal(batch)
	if err != nil {
		t.Fatal(err)
	}
	return body
}

func TestSafetyResultHasIndependentContractWithoutFuzzyScore(t *testing.T) {
	body := fixture(t, "fixtures/valid/result-safety-unavailable.json")
	if _, err := ValidateBatch(body); err != nil {
		t.Fatalf("score-free unavailable safety result rejected: %v", err)
	}

	withScore := safetyFixture(t, map[string]any{
		"state": "UNAVAILABLE", "reason": "SAFETY_NOT_CONFIGURED",
		"model_state": "UNCONFIGURED", "trace": nil, "score": nil,
	})
	if _, err := ValidateBatch(withScore); err == nil {
		t.Fatal("safety score must not be accepted by the independent contract")
	}
}

func TestSafetyResultRejectsFuzzyOnlyStates(t *testing.T) {
	for _, state := range []string{"SAFE", "DEGRADED"} {
		body := safetyFixture(t, map[string]any{
			"state": state, "reason": "READY", "model_state": "TEST_ONLY", "trace": nil,
		})
		if _, err := ValidateBatch(body); err == nil {
			t.Fatalf("safety state %q must be rejected", state)
		}
	}
}

func TestSafetyHazardInputCannotBecomeClear(t *testing.T) {
	body := safetyFixture(t, map[string]any{
		"state": "CLEAR", "reason": "HAZARD_INPUT_INVALID",
		"model_state": "TEST_ONLY", "trace": nil,
	})
	if _, err := ValidateBatch(body); err == nil {
		t.Fatal("invalid hazard input must not be represented as CLEAR")
	}
}

func TestSafetyFaultIsRepresentableWithoutScore(t *testing.T) {
	body := safetyFixture(t, map[string]any{
		"state": "FAULT", "reason": "HAZARD_INPUT_INVALID",
		"model_state": "TEST_ONLY", "trace": nil,
	})
	if _, err := ValidateBatch(body); err != nil {
		t.Fatalf("safety FAULT result rejected: %v", err)
	}
}
