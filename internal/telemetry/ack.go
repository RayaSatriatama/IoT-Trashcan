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
	var ack Ack
	if err := json.Unmarshal(raw, &ack); err != nil {
		return Ack{}, contractError(MalformedJSON, "", err.Error())
	}
	if ack.Status != "DURABLE" || ack.DeviceID != expected.DeviceID || ack.BatchID != expected.BatchID || ack.BodyHash != expected.BodyHash || ack.RecordCount != expected.RecordCount || ack.StoredCount+ack.DeduplicatedCount != ack.RecordCount {
		return Ack{}, contractError(AckMismatch, "", "ACK identity, status, hash, or counts do not match")
	}
	return ack, nil
}
