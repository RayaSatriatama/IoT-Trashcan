// Package safety evaluates explicit operational-safety conditions separately
// from the fuzzy decision engine.
package safety

import "github.com/RayaSatriatama/IoT-Trashcan/internal/device"

type Signal string

const (
	SignalFill        Signal = "fill"
	SignalWeight      Signal = "weight"
	SignalTemperature Signal = "temperature"
	SignalGasRaw      Signal = "gas_raw"
)

const (
	StateClear        = "Clear"
	StateSafetyAlert  = "Safety Alert"
	StateUnconfigured = "Unconfigured"
)

// Input contains raw operational values. Values remain optional to preserve
// the difference between a missing reading and numeric zero.
type Input struct {
	Fill        *float64
	Weight      *float64
	Temperature *float64
	GasRaw      *float64
}

// Threshold is an explicitly supplied safety boundary. Task 02 deliberately
// defines no production thresholds; callers must supply domain-supported
// values at composition time.
type Threshold struct {
	Signal    Signal
	AtOrAbove float64
	Reason    string
}

type Config struct {
	Thresholds []Threshold
}

type Result struct {
	Configured bool
	Alert      bool
	State      string
	Reason     string
}

// Evaluate checks supplied thresholds only. An absent config is represented
// explicitly as Unconfigured and can never manufacture a safety alert.
func Evaluate(input Input, config Config) Result {
	if len(config.Thresholds) == 0 {
		return Result{State: StateUnconfigured}
	}

	for _, threshold := range config.Thresholds {
		value := input.value(threshold.Signal)
		if value != nil && *value >= threshold.AtOrAbove {
			return Result{
				Configured: true,
				Alert:      true,
				State:      StateSafetyAlert,
				Reason:     threshold.Reason,
			}
		}
	}

	return Result{Configured: true, State: StateClear}
}

// Resolve applies the safety override while leaving a FIS status unchanged
// when safety is not configured or its conditions are not met.
func Resolve(fisStatus device.Status, result Result) device.Status {
	if result.Alert {
		return device.StatusSafetyAlert
	}
	return fisStatus
}

func (input Input) value(signal Signal) *float64 {
	switch signal {
	case SignalFill:
		return input.Fill
	case SignalWeight:
		return input.Weight
	case SignalTemperature:
		return input.Temperature
	case SignalGasRaw:
		return input.GasRaw
	default:
		return nil
	}
}
