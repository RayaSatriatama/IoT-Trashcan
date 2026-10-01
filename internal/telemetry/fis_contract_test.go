package telemetry

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestFuzzyResultUsesDedicatedStateAndLabelContract(t *testing.T) {
	validUnavailable := json.RawMessage(`{"state":"UNAVAILABLE","score":null,"label":null,"reason":"INPUT_MISSING","model_state":"UNCONFIGURED","trace":null}`)
	if err := validateFuzzyResult(validUnavailable, "fuzzy"); err != nil {
		t.Fatalf("unavailable fuzzy result rejected: %v", err)
	}

	validReady := json.RawMessage(`{"state":"READY","score":32.56,"label":"Normal","reason":"READY","model_state":"TEST_ONLY","trace":{"model_ref":{"version":"1","hash":"sha256:` + strings.Repeat("0", 64) + `"},"memberships":[],"rules":[],"aggregate":{"output_id":"normal","area":1.0,"centroid":32.56}}}`)
	if err := validateFuzzyResult(validReady, "fuzzy"); err != nil {
		t.Fatalf("ready fuzzy result rejected: %v", err)
	}
}

func TestFuzzyResultRejectsSafetyOnlyStatesAndMissingEvidence(t *testing.T) {
	for name, raw := range map[string]json.RawMessage{
		"safe-state":      json.RawMessage(`{"state":"SAFE","score":0.1,"label":"Safe","reason":"READY","model_state":"TEST_ONLY","trace":null}`),
		"ready-no-label":  json.RawMessage(`{"state":"READY","score":32.56,"label":null,"reason":"READY","model_state":"TEST_ONLY","trace":null}`),
		"trace-no-output": json.RawMessage(`{"state":"READY","score":32.56,"label":"Normal","reason":"READY","model_state":"TEST_ONLY","trace":{"model_ref":{"version":"1","hash":"sha256:` + strings.Repeat("0", 64) + `"},"memberships":[],"rules":[],"aggregate":{"area":1.0,"centroid":32.56}}}`),
	} {
		t.Run(name, func(t *testing.T) {
			if err := validateFuzzyResult(raw, "fuzzy"); err == nil {
				t.Fatal("expected fuzzy contract rejection")
			}
		})
	}
}
