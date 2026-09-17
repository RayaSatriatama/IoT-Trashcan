package simulator

import "testing"

func TestScenarioCycleCoversAllDemoStates(t *testing.T) {
	want := []Phase{PhaseNormal, PhaseWaspada, PhaseAngkat, PhaseSafetyAlert, PhaseDataInvalid, PhaseOffline, PhaseOffline, PhaseOffline, PhaseOffline, PhaseRecovery}
	for tick, expected := range want {
		if got := phaseForTick(uint64(tick)); got != expected {
			t.Fatalf("tick %d phase=%q want=%q", tick, got, expected)
		}
	}
	if got := phaseForTick(10); got != PhaseNormal {
		t.Fatalf("cycle did not restart: phase=%q", got)
	}
}
