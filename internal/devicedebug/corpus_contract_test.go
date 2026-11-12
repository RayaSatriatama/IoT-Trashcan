package devicedebug

import (
	"encoding/hex"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

type invalidCorpus struct {
	Format   string `json:"format"`
	Version  int    `json:"version"`
	Encoding string `json:"encoding"`
	Cases    []struct {
		Target string `json:"target"`
		Name   string `json:"name"`
		RawHex string `json:"raw_hex"`
	} `json:"cases"`
}

func TestDecodeUploadRejectsSharedInvalidCorpus(t *testing.T) {
	raw, err := os.ReadFile("../../contracts/debug/v1/fixtures/invalid.json")
	if err != nil {
		t.Fatal(err)
	}
	var corpus invalidCorpus
	if err := json.Unmarshal(raw, &corpus); err != nil {
		t.Fatal(err)
	}
	if corpus.Format != "sigap-debug-invalid-corpus" || corpus.Version != 1 || corpus.Encoding != "raw bytes as lowercase hexadecimal" {
		t.Fatalf("unexpected invalid corpus format/version/encoding: %#v", corpus)
	}
	names := make(map[string]bool, len(corpus.Cases))
	targets := map[string]int{"ble": 0, "upload": 0, "ack": 0}
	for _, entry := range corpus.Cases {
		if entry.Name == "" || names[entry.Name] {
			t.Fatalf("empty or duplicate corpus case name %q", entry.Name)
		}
		names[entry.Name] = true
		if _, ok := targets[entry.Target]; !ok {
			t.Fatalf("unknown corpus target %q for %s", entry.Target, entry.Name)
		}
		targets[entry.Target]++
		if !strings.EqualFold(entry.RawHex, strings.ToLower(entry.RawHex)) || len(entry.RawHex)%2 != 0 {
			t.Fatalf("%s: raw_hex must be even-length lowercase hex", entry.Name)
		}
		bytes, err := hex.DecodeString(entry.RawHex)
		if err != nil || hex.EncodeToString(bytes) != entry.RawHex {
			t.Fatalf("%s: invalid exact-byte hex: %v", entry.Name, err)
		}
		if entry.Target == "upload" {
			if _, err := DecodeUpload(bytes); err == nil {
				t.Errorf("%s: DecodeUpload accepted invalid bytes", entry.Name)
			}
		}
	}
	for target, count := range targets {
		if count == 0 {
			t.Errorf("invalid corpus has no %s cases", target)
		}
	}
}
