// Package safety evaluates explicit operational-safety conditions separately
// from the fuzzy decision engine.
package safety

import (
	"errors"
	"fmt"
	"math"
	"strings"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/device"
)

var ErrInvalidConfig = errors.New("invalid safety configuration")

type Signal string

const (
	SignalFill               Signal = "fill"
	SignalWeight             Signal = "weight"
	SignalTemperature        Signal = "temperature"
	SignalGasRaw             Signal = "gas_raw"
	SignalMQ7CO              Signal = "mq7_co"
	SignalMQ8H2              Signal = "mq8_h2"
	SignalDS18B20Temperature Signal = "ds18b20_temperature"
)

const (
	StateClear        = "Clear"
	StateSafetyAlert  = "Safety Alert"
	StateUnavailable  = "Unavailable"
	StateUnconfigured = "Unconfigured"
)

// Input contains raw operational values. Values remain optional to preserve
// the difference between a missing reading and numeric zero.
type Input struct {
	Fill               *float64
	Weight             *float64
	Temperature        *float64
	GasRaw             *float64
	MQ7CO              *float64
	MQ8H2              *float64
	DS18B20Temperature *float64
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
	Evaluated  bool
	Alert      bool
	State      string
	Reason     string
}

// Evaluate checks supplied thresholds only. An absent config is represented
// explicitly as Unconfigured. Malformed configuration is rejected, and a
// configured threshold with a missing/non-finite input is Unavailable rather
// than incorrectly reported as Clear.
func Evaluate(input Input, config Config) (Result, error) {
	if len(config.Thresholds) == 0 {
		return Result{State: StateUnconfigured}, nil
	}
	if err := validateConfig(config); err != nil {
		return Result{}, err
	}

	unavailable := false
	for _, threshold := range config.Thresholds {
		value := input.value(threshold.Signal)
		if value == nil || !finite(*value) {
			unavailable = true
			continue
		}
		if *value >= threshold.AtOrAbove {
			return Result{
				Configured: true,
				Evaluated:  true,
				Alert:      true,
				State:      StateSafetyAlert,
				Reason:     threshold.Reason,
			}, nil
		}
	}

	if unavailable {
		return Result{Configured: true, State: StateUnavailable}, nil
	}
	return Result{Configured: true, Evaluated: true, State: StateClear}, nil
}

// Resolve applies the safety override while leaving a FIS status unchanged
// when safety is not configured, unavailable, or its conditions are not met.
func Resolve(fisStatus device.Status, result Result) device.Status {
	if result.Alert {
		return device.StatusSafetyAlert
	}
	return fisStatus
}

func validateConfig(config Config) error {
	for index, threshold := range config.Thresholds {
		if !validSignal(threshold.Signal) {
			return fmt.Errorf("%w: threshold %d signal %q", ErrInvalidConfig, index, threshold.Signal)
		}
		if !finite(threshold.AtOrAbove) {
			return fmt.Errorf("%w: threshold %d boundary", ErrInvalidConfig, index)
		}
		if strings.TrimSpace(threshold.Reason) == "" {
			return fmt.Errorf("%w: threshold %d reason", ErrInvalidConfig, index)
		}
	}
	return nil
}

func validSignal(signal Signal) bool {
	switch signal {
	case SignalFill, SignalWeight, SignalTemperature, SignalGasRaw, SignalMQ7CO, SignalMQ8H2, SignalDS18B20Temperature:
		return true
	default:
		return false
	}
}

func finite(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0)
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
	case SignalMQ7CO:
		return input.MQ7CO
	case SignalMQ8H2:
		return input.MQ8H2
	case SignalDS18B20Temperature:
		return input.DS18B20Temperature
	default:
		return nil
	}
}
