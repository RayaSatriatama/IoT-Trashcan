package ingest

import "fmt"

// Normalize applies source-backed values only. OGI and accumulation are copied
// when the producer explicitly supplies them and are never inferred here.
func Normalize(slot Slot, operationalCapacity *float64) (NormalizedReading, error) {
	normalized := NormalizedReading{
		FillPct:           slot.Fill,
		OGI:               slot.OGI,
		AccumulationHours: slot.AccumulationHours,
		Temperature:       slot.Temperature,
	}
	if slot.Weight == nil {
		return normalized, nil
	}
	if operationalCapacity == nil {
		return normalized, nil
	}
	if *operationalCapacity <= 0 {
		return NormalizedReading{}, fmt.Errorf("%w: must be positive", ErrInvalidCapacity)
	}
	utilization := 100 * *slot.Weight / *operationalCapacity
	normalized.WeightUtilization = &utilization
	return normalized, nil
}
