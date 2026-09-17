package safety

import (
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/device"
)

func TestEvaluateWithoutExplicitConfigDoesNotAlert(t *testing.T) {
	value := 999.0
	result := Evaluate(Input{GasRaw: &value}, Config{})
	if result.Configured || result.Alert || result.State != StateUnconfigured {
		t.Fatalf("Evaluate() = %#v, want unconfigured non-alert", result)
	}
	if got := Resolve(device.StatusAngkat, result); got != device.StatusAngkat {
		t.Fatalf("Resolve() = %q, want Angkat", got)
	}
}

func TestSafetyAlertOverridesFISStatus(t *testing.T) {
	value := 80.0
	config := Config{Thresholds: []Threshold{{
		Signal:    SignalGasRaw,
		AtOrAbove: 50,
		Reason:    "test-only gas fixture",
	}}}
	result := Evaluate(Input{GasRaw: &value}, config)
	if !result.Configured || !result.Alert || result.State != StateSafetyAlert {
		t.Fatalf("Evaluate() = %#v, want configured safety alert", result)
	}
	if result.Reason != "test-only gas fixture" {
		t.Fatalf("reason = %q", result.Reason)
	}

	for _, fis := range []device.Status{device.StatusNormal, device.StatusWaspada, device.StatusAngkat} {
		if got := Resolve(fis, result); got != device.StatusSafetyAlert {
			t.Fatalf("Resolve(%q) = %q, want Safety Alert", fis, got)
		}
	}
}

func TestNonAlertSafetyPreservesFISStatus(t *testing.T) {
	value := 49.0
	result := Evaluate(Input{GasRaw: &value}, Config{Thresholds: []Threshold{{
		Signal: SignalGasRaw, AtOrAbove: 50, Reason: "test-only gas fixture",
	}}})
	if result.Alert || result.State != StateClear {
		t.Fatalf("Evaluate() = %#v, want clear", result)
	}
	if got := Resolve(device.StatusWaspada, result); got != device.StatusWaspada {
		t.Fatalf("Resolve() = %q, want Waspada", got)
	}
}
