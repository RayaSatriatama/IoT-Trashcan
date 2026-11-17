package devicecontrol

import (
	"bytes"
	"encoding/json"
	"os"
	"testing"
)

func TestMQ135GasResultsAcceptActualProducerFixtures(t *testing.T) {
	fixtures := []string{
		"above-range.json",
		"adc-unavailable.json",
		"age-at-inclusive-boundary.json",
		"below-range.json",
		"in-range.json",
		"invalid-ratio.json",
		"legacy-mqg1.json",
		"operational-baseline.json",
		"stale-source.json",
		"source-identity-invalid.json",
		"source-identity-unavailable.json",
		"worst-case-valid.json",
		"negative/adc-saturated.json",
		"negative/age-over-boundary.json",
		"negative/raw-absent.json",
		"negative/sequence-zero.json",
		"negative/source-profile-mismatch.json",
	}
	for _, fixture := range fixtures {
		t.Run(fixture, func(t *testing.T) {
			payload, err := os.ReadFile("../../contracts/calibration/mq135-nh3/v2/fixtures/" + fixture)
			if err != nil {
				t.Fatal(err)
			}
			if !validMq135GasResult(payload) {
				t.Fatal("valid producer fixture rejected")
			}
		})
	}
}

func TestMQ135GasResultRejectsInvalidProducerStress(t *testing.T) {
	payload, err := os.ReadFile("../../contracts/calibration/mq135-nh3/v2/fixtures/negative/serializer-stress-invalid.json")
	if err != nil {
		t.Fatal(err)
	}
	if validMq135GasResult(payload) {
		t.Fatal("contradictory invalid stress document accepted")
	}
}

func TestMQG2GasResultRejectsMalformedAndContradictoryEvidence(t *testing.T) {
	payload, err := os.ReadFile("../../contracts/calibration/mq135-nh3/v2/fixtures/in-range.json")
	if err != nil {
		t.Fatal(err)
	}
	outsideRatio := bytes.ReplaceAll(payload, []byte("0.29982668977469662"), []byte("0.6559000000000001"))
	for name, changed := range map[string][]byte{
		"duplicate root key":                       bytes.Replace(payload, []byte(`"schema":"MQG2"`), []byte(`"schema":"MQG2","schema":"MQG2"`), 1),
		"unknown nested key":                       bytes.Replace(payload, []byte(`"raw_counts":2308`), []byte(`"raw_counts":2308,"unexpected":true`), 1),
		"non-finite number":                        bytes.Replace(payload, []byte(`"pin_mv":2308`), []byte(`"pin_mv":NaN`), 1),
		"non-canonical uint64":                     bytes.Replace(payload, []byte(`"sequence":"4"`), []byte(`"sequence":"04"`), 1),
		"non-null ppm outside strict curve domain": outsideRatio,
	} {
		t.Run(name, func(t *testing.T) {
			if bytes.Equal(payload, changed) || validMq135GasResult(changed) {
				t.Fatal("malformed or contradictory MQG2 document accepted")
			}
		})
	}
}

// Regression inputs below mutate the immutable in-range producer fixture; they
// characterize producer states and are not claims of new producer emissions.
func mq135MQG2DiagnosticInput(t *testing.T, reason, quality string, edit func(map[string]any)) []byte {
	t.Helper()
	payload, err := os.ReadFile("../../contracts/calibration/mq135-nh3/v2/fixtures/in-range.json")
	if err != nil {
		t.Fatal(err)
	}
	var root map[string]any
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.UseNumber()
	if err := decoder.Decode(&root); err != nil {
		t.Fatal(err)
	}
	root["quality"], root["reason"] = quality, reason
	root["adc"].(map[string]any)["ao_mv"] = nil
	root["response"].(map[string]any)["q"] = nil
	root["response"].(map[string]any)["ratio"] = nil
	root["nh3"].(map[string]any)["ppm"] = nil
	root["nh3"].(map[string]any)["status"] = "SOURCE_NOT_READY"
	if edit != nil {
		edit(root)
	}
	encoded, err := json.Marshal(root)
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}

func mutateMQ135MQG2Input(t *testing.T, payload []byte, edit func(map[string]any)) []byte {
	t.Helper()
	var root map[string]any
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.UseNumber()
	if err := decoder.Decode(&root); err != nil {
		t.Fatal(err)
	}
	edit(root)
	encoded, err := json.Marshal(root)
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}

func TestMQG2AcceptsSourceIdentityDiagnostics(t *testing.T) {
	cases := []struct {
		name    string
		reason  string
		quality string
		edit    func(map[string]any)
	}{
		{name: "invalid identity with ready source quality", reason: "SOURCE_IDENTITY_INVALID", quality: "INVALID"},
		{name: "identity failure precedes non-ready source quality", reason: "SOURCE_IDENTITY_INVALID", quality: "INVALID", edit: func(root map[string]any) {
			root["source"].(map[string]any)["quality"] = "WARMING_UP"
		}},
		{name: "unavailable joint identity retains known sample quality", reason: "SOURCE_IDENTITY_INVALID", quality: "INVALID", edit: func(root map[string]any) {
			source := root["source"].(map[string]any)
			for _, key := range []string{"boot_id", "sequence", "monotonic_us", "age_us", "max_age_us", "fresh"} {
				source[key] = nil
			}
		}},
		{name: "unknown source clock does not claim age", reason: "SOURCE_CLOCK_UNKNOWN", quality: "INVALID", edit: func(root map[string]any) {
			source := root["source"].(map[string]any)
			source["monotonic_us"] = "4101"
			for _, key := range []string{"age_us", "max_age_us", "fresh"} {
				source[key] = nil
			}
		}},
		{name: "absent source remains distinct", reason: "NO_SOURCE", quality: "DISCONNECTED", edit: func(root map[string]any) {
			source := root["source"].(map[string]any)
			for _, key := range []string{"boot_id", "sequence", "monotonic_us", "age_us", "max_age_us", "fresh", "quality"} {
				source[key] = nil
			}
		}},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			payload := mq135MQG2DiagnosticInput(t, test.reason, test.quality, test.edit)
			if !validMq135GasResult(payload) {
				t.Fatal("faithful source-identity diagnostic rejected")
			}
		})
	}
}

func TestMQG2RejectsForgedSourceIdentityDiagnostics(t *testing.T) {
	base := mq135MQG2DiagnosticInput(t, "SOURCE_IDENTITY_INVALID", "INVALID", nil)
	for name, edit := range map[string]func(map[string]any){
		"ready root quality": func(root map[string]any) { root["quality"] = "READY" },
		"ordinary source-not-ready reason with ready quality and no identity": func(root map[string]any) {
			root["reason"] = "SOURCE_NOT_READY"
			source := root["source"].(map[string]any)
			for _, key := range []string{"boot_id", "sequence", "monotonic_us", "age_us", "max_age_us", "fresh"} {
				source[key] = nil
			}
		},
		"AO value": func(root map[string]any) { root["adc"].(map[string]any)["ao_mv"] = json.Number("3846.666748046875") },
		"q value":  func(root map[string]any) { root["response"].(map[string]any)["q"] = json.Number("0.29982668977469662") },
		"ratio value": func(root map[string]any) {
			root["response"].(map[string]any)["ratio"] = json.Number("0.29982668977469662")
		},
		"ppm value": func(root map[string]any) { root["nh3"].(map[string]any)["ppm"] = json.Number("76.392519691621473") },
	} {
		t.Run(name, func(t *testing.T) {
			forged := mutateMQ135MQG2Input(t, base, edit)
			if validMq135GasResult(forged) {
				t.Fatal("forged source-identity diagnostic accepted")
			}
		})
	}
}
