package ingest

import (
	"fmt"
	"math"
	"strings"
)

// ValidateOperational checks only source-supported sensor invariants. It is
// deliberately separate from structural request validation so invalid data can
// remain auditable with a Data Invalid decision.
func ValidateOperational(slot Slot) ValidationResult {
	reasons := make([]string, 0, 2+len(slot.Sensors))
	if slot.Fill != nil && (*slot.Fill < 0 || *slot.Fill > 100) {
		reasons = append(reasons, "fill must be between 0 and 100")
	}
	if slot.Weight != nil && *slot.Weight < 0 {
		reasons = append(reasons, "weight must not be negative")
	}
	if slot.OGI != nil && (*slot.OGI < 0 || *slot.OGI > 100) {
		reasons = append(reasons, "ogi must be between 0 and 100")
	}
	if slot.AccumulationHours != nil && *slot.AccumulationHours < 0 {
		reasons = append(reasons, "accumulation_hours must not be negative")
	}
	for index, sensor := range slot.Sensors {
		label := "sensor[" + sensorLabel(index, sensor) + "]"
		if !validSensorState(sensor.SensorState) {
			reasons = append(reasons, label+".sensor_state is invalid")
		}
		if strings.EqualFold(strings.TrimSpace(sensor.Unit), "ppm") && (sensor.EngineeringValue == nil || strings.TrimSpace(sensor.CalibrationVersion) == "") {
			reasons = append(reasons, label+" ppm requires engineering_value and calibration_version")
		}
		if sensor.RawValue != nil && !finiteSensorValue(*sensor.RawValue) {
			reasons = append(reasons, label+".raw_value must be finite")
		}
		if sensor.EngineeringValue != nil && !finiteSensorValue(*sensor.EngineeringValue) {
			reasons = append(reasons, label+".engineering_value must be finite")
		}
		if sensor.NormalizedValue != nil && (!finiteSensorValue(*sensor.NormalizedValue) || *sensor.NormalizedValue < 0 || *sensor.NormalizedValue > 1) {
			reasons = append(reasons, label+".normalized_value must be between 0 and 1")
		}
	}
	return ValidationResult{Valid: len(reasons) == 0, Reasons: reasons}
}

func finiteSensorValue(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0)
}

func validSensorState(state SensorState) bool {
	switch state {
	case SensorValid, SensorWarmingUp, SensorStale, SensorInvalid, SensorDisabled:
		return true
	default:
		return false
	}
}

func sensorLabel(index int, sensor SensorReading) string {
	if strings.TrimSpace(sensor.SensorID) != "" {
		return sensor.SensorID
	}
	return fmt.Sprintf("%d", index)
}
