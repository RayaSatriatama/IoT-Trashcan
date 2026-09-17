package simulator

import (
	"testing"
	"time"
)

func TestParseConfigDefaultsToDisabled(t *testing.T) {
	cfg, err := ParseConfig(func(string) (string, bool) { return "", false })
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Enabled || cfg.Interval != 0 || cfg.Seed != defaultSeed {
		t.Fatalf("default config=%+v", cfg)
	}
}

func TestParseConfigEnabledUsesSimulationDefaults(t *testing.T) {
	cfg, err := ParseConfig(mapLookup(map[string]string{
		"SIGAP_SIMULATION_ENABLED": "true",
	}))
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.Enabled || cfg.Interval != 5*time.Second || cfg.Seed != defaultSeed {
		t.Fatalf("enabled config=%+v", cfg)
	}
}

func TestParseConfigRejectsInvalidSimulationValues(t *testing.T) {
	cases := []map[string]string{
		{"SIGAP_SIMULATION_ENABLED": "sometimes"},
		{"SIGAP_SIMULATION_ENABLED": "true", "SIGAP_SIMULATION_INTERVAL_SECONDS": "0"},
		{"SIGAP_SIMULATION_ENABLED": "true", "SIGAP_SIMULATION_INTERVAL_SECONDS": "fast"},
		{"SIGAP_SIMULATION_ENABLED": "true", "SIGAP_SIMULATION_SEED": "random"},
	}
	for _, values := range cases {
		if _, err := ParseConfig(mapLookup(values)); err == nil {
			t.Fatalf("ParseConfig(%v) accepted invalid values", values)
		}
	}
}

func mapLookup(values map[string]string) func(string) (string, bool) {
	return func(key string) (string, bool) {
		value, ok := values[key]
		return value, ok
	}
}
