package telemetry

import "encoding/json"

type ConfigRef struct {
	SchemaVersion uint32 `json:"schema_version"`
	ConfigVersion string `json:"config_version"`
	ConfigHash    string `json:"config_hash"`
}

type ProfileRef struct {
	Kind          string `json:"kind"`
	Target        string `json:"target"`
	SchemaVersion uint32 `json:"schema_version"`
	Version       string `json:"version"`
	Hash          string `json:"hash"`
}

type CommonRecord struct {
	RecordID    string                     `json:"record_id"`
	RecordHash  string                     `json:"record_hash"`
	Kind        string                     `json:"kind"`
	DeviceID    string                     `json:"device_id"`
	BootID      string                     `json:"boot_id"`
	Sequence    string                     `json:"sequence"`
	MonotonicUS string                     `json:"monotonic_us"`
	RecordedAt  *string                    `json:"recorded_at"`
	TimeQuality string                     `json:"time_quality"`
	ConfigRef   ConfigRef                  `json:"config_ref"`
	ProfileRefs []ProfileRef               `json:"profile_refs"`
	RawObject   map[string]json.RawMessage `json:"-"`
}

type SampleRecord struct {
	CommonRecord
	SensorID  string          `json:"sensor_id"`
	Stream    string          `json:"stream"`
	Quality   string          `json:"quality"`
	Reason    string          `json:"reason"`
	Raw       json.RawMessage `json:"raw"`
	Converted json.RawMessage `json:"converted"`
}

type ResultRecord struct {
	CommonRecord
	Stream              string          `json:"stream"`
	ComputedMonotonicUS string          `json:"computed_monotonic_us"`
	SourceSampleIDs     []string        `json:"source_sample_ids"`
	Processed           json.RawMessage `json:"processed"`
	Fuzzy               json.RawMessage `json:"fuzzy"`
	Safety              json.RawMessage `json:"safety"`
}

type EventRecord struct {
	CommonRecord
	EventType string          `json:"event_type"`
	Payload   json.RawMessage `json:"payload"`
}

type MetadataRecord struct {
	CommonRecord
	MetadataType string          `json:"metadata_type"`
	Payload      json.RawMessage `json:"payload"`
}

type Batch struct {
	SchemaVersion uint32           `json:"schema_version"`
	DeviceID      string           `json:"device_id"`
	BatchID       string           `json:"batch_id"`
	RecordCount   uint16           `json:"record_count"`
	Samples       []SampleRecord   `json:"samples"`
	Results       []ResultRecord   `json:"results"`
	Events        []EventRecord    `json:"events"`
	Metadata      []MetadataRecord `json:"metadata"`
}

type Ack struct {
	DeviceID          string `json:"device_id"`
	BatchID           string `json:"batch_id"`
	BodyHash          string `json:"body_hash"`
	RecordCount       uint16 `json:"record_count"`
	StoredCount       uint16 `json:"stored_count"`
	DeduplicatedCount uint16 `json:"deduplicated_count"`
	Status            string `json:"status"`
}

type AckExpectation struct {
	DeviceID    string
	BatchID     string
	BodyHash    string
	RecordCount uint16
}
