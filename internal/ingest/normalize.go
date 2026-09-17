package ingest

import "fmt"

// Normalize applies the source-backed normalization mechanics only. OGI and
// accumulation stay unavailable until their authoritative definitions exist.
func Normalize(slot Slot, operationalCapacity *float64) (NormalizedReading, error) {
	normalized := NormalizedReading{
		FillPct:     slot.Fill,
		Temperature: slot.Temperature,
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
