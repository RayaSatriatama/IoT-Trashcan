package telemetry

type ReplayDecision string

const (
	ReplayNew       ReplayDecision = "NEW"
	ReplayIdentical ReplayDecision = "IDENTICAL_REPLAY"
)

// CompareBatchReplay is pure receipt logic for a future transactional service.
func CompareBatchReplay(existingBodyHash, incomingBodyHash string) (ReplayDecision, error) {
	if existingBodyHash == incomingBodyHash {
		return ReplayIdentical, nil
	}
	return ReplayNew, contractError(Conflict, "batch_id", "batch identity has a different exact body hash")
}

// CheckRecordConflicts checks all records before any future transaction writes one.
func CheckRecordConflicts(existing map[string]string, batch *Batch) error {
	if batch == nil {
		return contractError(MalformedJSON, "", "nil batch")
	}
	check := func(record CommonRecord) error {
		if hash, ok := existing[record.RecordID]; ok && hash != record.RecordHash {
			return contractError(Conflict, "record_id", "record identity has a different record hash")
		}
		return nil
	}
	for _, record := range batch.Samples {
		if err := check(record.CommonRecord); err != nil {
			return err
		}
	}
	for _, record := range batch.Results {
		if err := check(record.CommonRecord); err != nil {
			return err
		}
	}
	for _, record := range batch.Events {
		if err := check(record.CommonRecord); err != nil {
			return err
		}
	}
	for _, record := range batch.Metadata {
		if err := check(record.CommonRecord); err != nil {
			return err
		}
	}
	return nil
}
