package telemetry

import (
	"encoding/json"
)

// ValidateAck checks the server acknowledgement before a future journal can advance.
func ValidateAck(raw []byte, expected AckExpectation) (Ack, error) {
	object, err := strictJSON(raw, MaxRecordBytes)
	if err != nil {
		return Ack{}, err
	}
	if err := requireFields(object, map[string]bool{"device_id": true, "batch_id": true, "body_hash": true, "record_count": true, "stored_count": true, "deduplicated_count": true, "status": true}); err != nil {
		return Ack{}, err
	}
	for _, key := range []string{"device_id", "batch_id", "body_hash", "record_count", "stored_count", "deduplicated_count", "status"} {
		if _, err := require(object, key); err != nil {
			return Ack{}, err
		}
	}
	var ack Ack
	if err := json.Unmarshal(raw, &ack); err != nil {
		return Ack{}, contractError(MalformedJSON, "", err.Error())
	}
	if err := validateID(ack.DeviceID, "device_id"); err != nil {
		return Ack{}, err
	}
	if err := validateHash(ack.BodyHash, "body_hash"); err != nil {
		return Ack{}, err
	}
	if ack.RecordCount > MaxRecords || ack.StoredCount > MaxRecords || ack.DeduplicatedCount > MaxRecords {
		return Ack{}, contractError(AckMismatch, "", "ACK count outside contract bound")
	}
	if ack.StoredCount > ack.RecordCount || ack.DeduplicatedCount > ack.RecordCount ||
		uint64(ack.StoredCount)+uint64(ack.DeduplicatedCount) != uint64(ack.RecordCount) {
		return Ack{}, contractError(AckMismatch, "", "ACK stored and deduplicated counts do not match record count")
	}
	if ack.Status != "DURABLE" || ack.DeviceID != expected.DeviceID || ack.BatchID != expected.BatchID || ack.BodyHash != expected.BodyHash || ack.RecordCount != expected.RecordCount {
		return Ack{}, contractError(AckMismatch, "", "ACK identity, status, hash, or counts do not match")
	}
	return ack, nil
}
