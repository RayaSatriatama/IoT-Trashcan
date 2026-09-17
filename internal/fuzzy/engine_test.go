package fuzzy

import (
	"math"
	"testing"
)

// testConfig is deliberately test-only. Its numeric boundaries are not SIGAP
// production thresholds; production callers must supply an evidence-backed Config.
func testConfig() Config {
	return Config{
		Version: "test-fis-v1",
		Inputs: map[string]InputVariable{
			"fill": {Sets: map[string]Membership{
				"low":  TriangleMembership(0, 0, 50),
				"high": TriangleMembership(50, 100, 100),
			}},
		},
		Outputs: map[string]OutputSet{
			"normal": {Status: "Normal", Membership: TriangleMembership(0, 0, 50)},
			"angkat": {Status: "Angkat", Membership: TriangleMembership(50, 100, 100)},
		},
		Rules: []Rule{
			{ID: "R-low", Conditions: []Condition{{Variable: "fill", Set: "low"}}, Output: "normal", Reason: "fill is low"},
			{ID: "R-high", Conditions: []Condition{{Variable: "fill", Set: "high"}}, Output: "angkat", Reason: "fill is high"},
		},
		OutputRange: Range{Min: 0, Max: 100, Step: 0.1},
	}
}

func TestTriangleMembershipBoundariesAndShoulders(t *testing.T) {
	tests := []struct {
		name       string
		membership Membership
		x, want    float64
	}{
		{"outside left", TriangleMembership(10, 20, 30), 9, 0},
		{"left slope", TriangleMembership(10, 20, 30), 15, 0.5},
		{"peak", TriangleMembership(10, 20, 30), 20, 1},
		{"right slope", TriangleMembership(10, 20, 30), 25, 0.5},
		{"outside right", TriangleMembership(10, 20, 30), 31, 0},
		{"left shoulder", TriangleMembership(0, 0, 10), 0, 1},
		{"right shoulder", TriangleMembership(0, 10, 10), 10, 1},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := tt.membership.Degree(tt.x); math.Abs(got-tt.want) > 1e-9 {
				t.Fatalf("Degree(%v) = %v, want %v", tt.x, got, tt.want)
			}
		})
	}
}

func TestTrapezoidMembershipBoundariesAndShoulders(t *testing.T) {
	tests := []struct {
		name       string
		membership Membership
		x, want    float64
	}{
		{"outside", TrapezoidMembership(10, 20, 30, 40), 5, 0},
		{"rising", TrapezoidMembership(10, 20, 30, 40), 15, 0.5},
		{"plateau start", TrapezoidMembership(10, 20, 30, 40), 20, 1},
		{"plateau end", TrapezoidMembership(10, 20, 30, 40), 30, 1},
		{"falling", TrapezoidMembership(10, 20, 30, 40), 35, 0.5},
		{"left shoulder", TrapezoidMembership(0, 0, 10, 20), 0, 1},
		{"right shoulder", TrapezoidMembership(0, 10, 20, 20), 20, 1},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := tt.membership.Degree(tt.x); math.Abs(got-tt.want) > 1e-9 {
				t.Fatalf("Degree(%v) = %v, want %v", tt.x, got, tt.want)
			}
		})
	}
}

func TestEngineEvaluatesRuleAndReturnsVersionAndReason(t *testing.T) {
	engine, err := NewEngine(testConfig())
	if err != nil {
		t.Fatal(err)
	}

	result, err := engine.Evaluate(Input{"fill": 90})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != "Angkat" {
		t.Fatalf("Status = %q, want Angkat", result.Status)
	}
	if result.DominantReason != "fill is high" {
		t.Fatalf("DominantReason = %q", result.DominantReason)
	}
	if result.Version != "test-fis-v1" {
		t.Fatalf("Version = %q", result.Version)
	}
	if len(result.Rules) != 2 || result.Rules[1].Strength <= result.Rules[0].Strength {
		t.Fatalf("Rules = %#v, want high rule dominant", result.Rules)
	}
}

func TestEngineAggregatesOutputsUsingMaximumMembership(t *testing.T) {
	cfg := testConfig()
	cfg.Rules = []Rule{
		{ID: "R-a", Conditions: []Condition{{Variable: "fill", Set: "high"}}, Output: "angkat", Reason: "first"},
		{ID: "R-b", Conditions: []Condition{{Variable: "fill", Set: "high"}}, Output: "angkat", Reason: "second"},
	}
	engine, err := NewEngine(cfg)
	if err != nil {
		t.Fatal(err)
	}
	result, err := engine.Evaluate(Input{"fill": 75})
	if err != nil {
		t.Fatal(err)
	}
	if got := result.Aggregated["angkat"].Degree(75); math.Abs(got-0.5) > 1e-9 {
		t.Fatalf("aggregated degree = %v, want 0.5 maximum clip", got)
	}
}

func TestEngineCentroidUsesAggregatedMembership(t *testing.T) {
	cfg := Config{
		Version:     "test-centroid",
		Inputs:      map[string]InputVariable{"signal": {Sets: map[string]Membership{"on": TriangleMembership(0, 1, 2)}}},
		Outputs:     map[string]OutputSet{"output": {Status: "Normal", Membership: TriangleMembership(0, 5, 10)}},
		Rules:       []Rule{{ID: "R", Conditions: []Condition{{Variable: "signal", Set: "on"}}, Output: "output", Reason: "fixture"}},
		OutputRange: Range{Min: 0, Max: 10, Step: 0.01},
	}
	engine, err := NewEngine(cfg)
	if err != nil {
		t.Fatal(err)
	}
	result, err := engine.Evaluate(Input{"signal": 1})
	if err != nil {
		t.Fatal(err)
	}
	// The centroid of this symmetric triangle is independently known as 5.
	if math.Abs(result.Score-5) > 0.02 {
		t.Fatalf("Score = %v, want approximately 5", result.Score)
	}
}

func TestNewEngineRejectsConfigurationWithoutRulesOrVersion(t *testing.T) {
	if _, err := NewEngine(Config{}); err == nil {
		t.Fatal("NewEngine accepted missing required configuration")
	}
}
