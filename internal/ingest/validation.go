package ingest

// ValidateOperational checks only source-supported sensor invariants. It is
// deliberately separate from structural request validation so invalid data can
// remain auditable with a Data Invalid decision.
func ValidateOperational(slot Slot) ValidationResult {
	reasons := make([]string, 0, 2)
	if slot.Fill != nil && (*slot.Fill < 0 || *slot.Fill > 100) {
		reasons = append(reasons, "fill must be between 0 and 100")
	}
	if slot.Weight != nil && *slot.Weight < 0 {
		reasons = append(reasons, "weight must not be negative")
	}
	return ValidationResult{Valid: len(reasons) == 0, Reasons: reasons}
}
