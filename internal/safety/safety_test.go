package safety

import (
	"errors"
	"math"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/device"
)

func TestEvaluateWithoutExplicitConfigDoesNotAlert(t *testing.T) {
	value := 999.0
	result, err := Evaluate(Input{GasRaw: &value}, Config{})
	if err != nil {
		t.Fatal(err)
	}
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
	result, err := Evaluate(Input{GasRaw: &value}, config)
	if err != nil {
		t.Fatal(err)
	}
	if !result.Configured || !result.Evaluated || !result.Alert || result.State != StateSafetyAlert {
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
	result, err := Evaluate(Input{GasRaw: &value}, Config{Thresholds: []Threshold{{
		Signal: SignalGasRaw, AtOrAbove: 50, Reason: "test-only gas fixture",
	}}})
	if err != nil {
		t.Fatal(err)
	}
	if result.Alert || !result.Configured || !result.Evaluated || result.State != StateClear {
		t.Fatalf("Evaluate() = %#v, want evaluated clear", result)
	}
	if got := Resolve(device.StatusWaspada, result); got != device.StatusWaspada {
		t.Fatalf("Resolve() = %q, want Waspada", got)
	}
}

func TestEvaluateRejectsMalformedConfiguration(t *testing.T) {
	tests := []struct {
		name      string
		threshold Threshold
	}{
		{
			name:      "unknown signal",
			threshold: Threshold{Signal: Signal("typo"), AtOrAbove: 10, Reason: "fixture"},
		},
		{
			name:      "nan boundary",
			threshold: Threshold{Signal: SignalGasRaw, AtOrAbove: math.NaN(), Reason: "fixture"},
		},
		{
			name:      "positive infinity boundary",
			threshold: Threshold{Signal: SignalGasRaw, AtOrAbove: math.Inf(1), Reason: "fixture"},
		},
		{
			name:      "negative infinity boundary",
			threshold: Threshold{Signal: SignalGasRaw, AtOrAbove: math.Inf(-1), Reason: "fixture"},
		},
		{
			name:      "missing reason",
			threshold: Threshold{Signal: SignalGasRaw, AtOrAbove: 10},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := Evaluate(Input{}, Config{Thresholds: []Threshold{tt.threshold}})
			if !errors.Is(err, ErrInvalidConfig) {
				t.Fatalf("Evaluate() error = %v, want ErrInvalidConfig", err)
			}
		})
	}
}

func TestEvaluateReportsUnavailableWhenConfiguredInputIsMissing(t *testing.T) {
	result, err := Evaluate(Input{}, Config{Thresholds: []Threshold{{
		Signal: SignalTemperature, AtOrAbove: 50, Reason: "test-only temperature fixture",
	}}})
	if err != nil {
		t.Fatal(err)
	}
	if !result.Configured || result.Evaluated || result.Alert || result.State != StateUnavailable {
		t.Fatalf("Evaluate() = %#v, want configured unavailable", result)
	}
}

func TestSafetyAlertWinsWhenAnotherConfiguredSignalIsUnavailable(t *testing.T) {
	gas := 80.0
	result, err := Evaluate(Input{GasRaw: &gas}, Config{Thresholds: []Threshold{
		{Signal: SignalTemperature, AtOrAbove: 50, Reason: "missing temperature fixture"},
		{Signal: SignalGasRaw, AtOrAbove: 50, Reason: "gas alert fixture"},
	}})
	if err != nil {
		t.Fatal(err)
	}
	if !result.Configured || !result.Evaluated || !result.Alert || result.State != StateSafetyAlert {
		t.Fatalf("Evaluate() = %#v, want Safety Alert", result)
	}
	if result.Reason != "gas alert fixture" {
		t.Fatalf("reason = %q, want gas alert fixture", result.Reason)
	}
}

func TestEvaluateSupportsExplicitB3SafetyChannels(t *testing.T) {
	co, hydrogen, temperature := 0.91, 0.12, 31.0
	result, err := Evaluate(Input{MQ7CO: &co, MQ8H2: &hydrogen, DS18B20Temperature: &temperature}, Config{Thresholds: []Threshold{
		{Signal: SignalMQ7CO, AtOrAbove: 0.8, Reason: "test CO alert"},
		{Signal: SignalMQ8H2, AtOrAbove: 0.8, Reason: "test H2 alert"},
		{Signal: SignalDS18B20Temperature, AtOrAbove: 55, Reason: "test temperature alert"},
	}})
	if err != nil {
		t.Fatal(err)
	}
	if !result.Alert || result.State != StateSafetyAlert || result.Reason != "test CO alert" {
		t.Fatalf("Evaluate() = %#v, want explicit MQ-7 alert", result)
	}
}
