package telemetry

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/lattice-substrate/json-canon/jcserr"
	"github.com/lattice-substrate/json-canon/jcstoken"
)

var (
	sha256Pattern = regexp.MustCompile(`^sha256:[0-9a-f]{64}$`)
	idPattern     = regexp.MustCompile(`^[A-Za-z0-9._-]{1,128}$`)
	profileTarget = regexp.MustCompile(`^[a-z0-9_]{1,128}$`)
)

var allowedBatchFields = map[string]bool{
	"schema_version": true, "device_id": true, "batch_id": true,
	"record_count": true, "samples": true, "results": true,
	"events": true, "metadata": true,
}

var commonFields = map[string]bool{
	"record_id": true, "record_hash": true, "kind": true, "device_id": true,
	"boot_id": true, "sequence": true, "monotonic_us": true,
	"recorded_at": true, "time_quality": true, "config_ref": true,
	"profile_refs": true,
}

var sampleFields = mergeFields(commonFields, map[string]bool{
	"sensor_id": true, "stream": true, "quality": true, "reason": true,
	"raw": true, "converted": true,
})

var resultFields = mergeFields(commonFields, map[string]bool{
	"stream": true, "computed_monotonic_us": true, "source_sample_ids": true,
	"processed": true, "fuzzy": true, "safety": true,
})

var eventFields = mergeFields(commonFields, map[string]bool{"event_type": true, "payload": true})
var metadataFields = mergeFields(commonFields, map[string]bool{"metadata_type": true, "payload": true})

var knownSensors = map[string]bool{
	"mq4": true, "mq135": true, "mq7": true, "mq8": true,
	"hx711_organic": true, "hx711_inorganic": true, "hx711_b3": true,
	"ultrasonic_organic": true, "ultrasonic_inorganic": true, "ultrasonic_b3": true,
	"ds18b20_b3": true, "sht31_44": true, "sht31_45": true,
}

var knownStreams = map[string]bool{"organic": true, "inorganic": true, "b3": true, "ambient": true}
var knownQualities = map[string]bool{
	"READY": true, "WARMING_UP": true, "HEATER_PHASE": true, "UNCALIBRATED": true,
	"CALIBRATING": true, "TIMEOUT": true, "DISCONNECTED": true, "CRC_ERROR": true,
	"STALE": true, "OUT_OF_RANGE": true,
}
var knownReasons = map[string]bool{
	"NONE": true, "DATA_READY": true, "NOT_READY": true, "TIMEOUT": true,
	"DISCONNECTED": true, "CRC_ERROR": true, "INVALID_PROFILE": true,
	"HEATER_PHASE": true, "CONVERSION_PENDING": true, "OUT_OF_RANGE": true,
	"PROFILE_MISSING": true, "INPUT_MISSING": true, "STALE": true,
}
var knownTimeQualities = map[string]bool{"UNSYNCED": true, "SYNCED": true, "RECONSTRUCTED": true}
var knownProfileKinds = map[string]bool{"CALIBRATION": true, "CAPACITY": true, "FIS": true, "SAFETY": true, "HEATER": true, "ADC": true}
var knownEventTypes = map[string]bool{"CLOCK_ANCHOR": true, "GAP": true, "RESET": true, "HEALTH_TRANSITION": true, "FAULT": true}
var knownMetadataTypes = map[string]bool{"CONFIG_ACTIVATION": true, "PROFILE_ACTIVATION": true, "BOOT": true, "HEALTH_SNAPSHOT": true}

func mergeFields(left, right map[string]bool) map[string]bool {
	merged := make(map[string]bool, len(left)+len(right))
	for key := range left {
		merged[key] = true
	}
	for key := range right {
		merged[key] = true
	}
	return merged
}

func strictJSON(raw []byte, limit int) (map[string]json.RawMessage, error) {
	if limit == MaxBodyBytes && len(raw) > MaxBodyBytes {
		return nil, contractError(BodyTooLarge, "", "request body exceeds 32 KiB")
	}
	if limit == MaxRecordBytes && len(raw) > MaxRecordBytes {
		return nil, contractError(RecordTooLarge, "", "record exceeds 16 KiB")
	}
	if !utf8.Valid(raw) {
		return nil, contractError(InvalidUTF8, "", "JSON input is not valid UTF-8")
	}
	options := strictOptions()
	options.MaxInputSize = limit
	_, err := jcstoken.ParseWithOptions(raw, options)
	if err != nil {
		return nil, translateJSONError(err, limit)
	}
	var object map[string]json.RawMessage
	if err := json.Unmarshal(raw, &object); err != nil {
		return nil, contractError(MalformedJSON, "", err.Error())
	}
	if object == nil {
		return nil, contractError(MalformedJSON, "", "expected JSON object")
	}
	return object, nil
}

func translateJSONError(err error, limit int) error {
	var jcsErr *jcserr.Error
	if errors.As(err, &jcsErr) {
		switch jcsErr.Class {
		case jcserr.InvalidUTF8:
			return contractError(InvalidUTF8, "", jcsErr.Error())
		case jcserr.DuplicateKey:
			return contractError(DuplicateKey, "", jcsErr.Error())
		case jcserr.NumberOverflow, jcserr.NumberUnderflow, jcserr.NumberNegZero:
			return contractError(NonFiniteNumber, "", jcsErr.Error())
		case jcserr.BoundExceeded:
			message := strings.ToLower(jcsErr.Error())
			if strings.Contains(message, "depth") {
				return contractError(DepthExceeded, "", jcsErr.Error())
			}
			if strings.Contains(message, "string") {
				return contractError(StringTooLong, "", jcsErr.Error())
			}
			if limit == MaxBodyBytes {
				return contractError(BodyTooLarge, "", jcsErr.Error())
			}
			if limit == MaxRecordBytes {
				return contractError(RecordTooLarge, "", jcsErr.Error())
			}
			return contractError(MalformedJSON, "", jcsErr.Error())
		default:
			return contractError(MalformedJSON, "", jcsErr.Error())
		}
	}
	return contractError(MalformedJSON, "", err.Error())
}

func requireFields(object map[string]json.RawMessage, fields map[string]bool) error {
	for key := range object {
		if !fields[key] {
			return contractError(UnknownField, key, "field is not part of the frozen contract")
		}
	}
	return nil
}

func require(object map[string]json.RawMessage, key string) (json.RawMessage, error) {
	value, ok := object[key]
	if !ok {
		return nil, contractError(MalformedJSON, key, "required field is missing")
	}
	return value, nil
}

func decodeString(object map[string]json.RawMessage, key string) (string, error) {
	raw, err := require(object, key)
	if err != nil {
		return "", err
	}
	var value string
	if err := json.Unmarshal(raw, &value); err != nil {
		return "", contractError(MalformedJSON, key, err.Error())
	}
	if len(value) > MaxFreeStringBytes {
		return "", contractError(StringTooLong, key, "free-form string exceeds 512 UTF-8 bytes")
	}
	return value, nil
}

func decodeUint64String(object map[string]json.RawMessage, key string) (string, error) {
	value, err := decodeString(object, key)
	if err != nil {
		return "", err
	}
	if _, err := ParseUint64Decimal(value); err != nil {
		return "", contractError(InvalidUint64, key, err.Error())
	}
	return value, nil
}

func decodeUint32(object map[string]json.RawMessage, key string) (uint32, error) {
	raw, err := require(object, key)
	if err != nil {
		return 0, err
	}
	var value uint32
	if err := json.Unmarshal(raw, &value); err != nil {
		return 0, contractError(MalformedJSON, key, err.Error())
	}
	return value, nil
}

func validateHash(value, path string) error {
	if !sha256Pattern.MatchString(value) {
		return contractError(HashMismatch, path, "expected sha256:<64 lowercase hex>")
	}
	return nil
}

func validateID(value, path string) error {
	if !idPattern.MatchString(value) {
		return contractError(InvalidIdentity, path, "ID must contain only ASCII identity characters")
	}
	return nil
}

func validateRecordID(value, expectedDevice, path string) error {
	parts := strings.Split(value, "/")
	if len(parts) != 3 || parts[0] != expectedDevice {
		return contractError(InvalidIdentity, path, "record ID must be device/boot/sequence")
	}
	if err := validateID(parts[0], path); err != nil {
		return err
	}
	if err := validateID(parts[1], path); err != nil {
		return err
	}
	if _, err := ParseUint64Decimal(parts[2]); err != nil {
		return contractError(InvalidIdentity, path, "record ID sequence is not canonical uint64")
	}
	return nil
}

func validateConfigRef(raw json.RawMessage) (ConfigRef, error) {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(raw, &object); err != nil || object == nil {
		return ConfigRef{}, contractError(MalformedJSON, "config_ref", "expected object")
	}
	if err := requireFields(object, map[string]bool{"schema_version": true, "config_version": true, "config_hash": true}); err != nil {
		return ConfigRef{}, err
	}
	schema, err := decodeUint32(object, "schema_version")
	if err != nil || schema != 1 {
		return ConfigRef{}, contractError(SchemaUnsupported, "config_ref.schema_version", "config schema version must be 1")
	}
	version, err := decodeUint64String(object, "config_version")
	if err != nil {
		return ConfigRef{}, err
	}
	hash, err := decodeString(object, "config_hash")
	if err != nil {
		return ConfigRef{}, err
	}
	if err := validateHash(hash, "config_ref.config_hash"); err != nil {
		return ConfigRef{}, err
	}
	return ConfigRef{SchemaVersion: schema, ConfigVersion: version, ConfigHash: hash}, nil
}

func validateProfileRefs(raw json.RawMessage) ([]ProfileRef, error) {
	var objects []map[string]json.RawMessage
	if err := json.Unmarshal(raw, &objects); err != nil || objects == nil {
		return nil, contractError(ProfileMalformed, "profile_refs", "expected an array")
	}
	refs := make([]ProfileRef, 0, len(objects))
	for index, object := range objects {
		path := fmt.Sprintf("profile_refs[%d]", index)
		if err := requireFields(object, map[string]bool{"kind": true, "target": true, "schema_version": true, "version": true, "hash": true}); err != nil {
			return nil, err
		}
		kind, err := decodeString(object, "kind")
		if err != nil || !knownProfileKinds[kind] {
			return nil, contractError(ProfileMalformed, path+".kind", "unknown profile kind")
		}
		target, err := decodeString(object, "target")
		if err != nil || !profileTarget.MatchString(target) {
			return nil, contractError(ProfileMalformed, path+".target", "invalid profile target")
		}
		schema, err := decodeUint32(object, "schema_version")
		if err != nil || schema == 0 {
			return nil, contractError(ProfileMalformed, path+".schema_version", "schema version must be positive")
		}
		version, err := decodeUint64String(object, "version")
		if err != nil {
			return nil, err
		}
		hash, err := decodeString(object, "hash")
		if err != nil {
			return nil, err
		}
		if err := validateHash(hash, path+".hash"); err != nil {
			return nil, err
		}
		refs = append(refs, ProfileRef{Kind: kind, Target: target, SchemaVersion: schema, Version: version, Hash: hash})
	}
	return refs, nil
}

func validateCommon(object map[string]json.RawMessage, expectedKind string) (CommonRecord, error) {
	for key := range commonFields {
		if _, err := require(object, key); err != nil {
			return CommonRecord{}, err
		}
	}
	recordID, err := decodeString(object, "record_id")
	if err != nil {
		return CommonRecord{}, err
	}
	recordHash, err := decodeString(object, "record_hash")
	if err != nil {
		return CommonRecord{}, err
	}
	if err := validateHash(recordHash, "record_hash"); err != nil {
		return CommonRecord{}, err
	}
	kind, err := decodeString(object, "kind")
	if err != nil || kind != expectedKind {
		return CommonRecord{}, contractError(InvalidEnum, "kind", "record kind does not match its array")
	}
	deviceID, err := decodeString(object, "device_id")
	if err != nil {
		return CommonRecord{}, err
	}
	bootID, err := decodeString(object, "boot_id")
	if err != nil {
		return CommonRecord{}, err
	}
	if err := validateID(deviceID, "device_id"); err != nil {
		return CommonRecord{}, err
	}
	if err := validateID(bootID, "boot_id"); err != nil {
		return CommonRecord{}, err
	}
	sequence, err := decodeUint64String(object, "sequence")
	if err != nil {
		return CommonRecord{}, err
	}
	monotonic, err := decodeUint64String(object, "monotonic_us")
	if err != nil {
		return CommonRecord{}, err
	}
	recordedAtRaw, err := require(object, "recorded_at")
	if err != nil {
		return CommonRecord{}, err
	}
	var recordedAt *string
	if !bytes.Equal(bytes.TrimSpace(recordedAtRaw), []byte("null")) {
		var value string
		if err := json.Unmarshal(recordedAtRaw, &value); err != nil {
			return CommonRecord{}, contractError(MalformedJSON, "recorded_at", err.Error())
		}
		if _, err := time.Parse(time.RFC3339Nano, value); err != nil {
			return CommonRecord{}, contractError(MalformedJSON, "recorded_at", "expected RFC3339 timestamp")
		}
		recordedAt = &value
	}
	timeQuality, err := decodeString(object, "time_quality")
	if err != nil || !knownTimeQualities[timeQuality] {
		return CommonRecord{}, contractError(InvalidEnum, "time_quality", "unknown time quality")
	}
	configRaw, err := require(object, "config_ref")
	if err != nil {
		return CommonRecord{}, err
	}
	configRef, err := validateConfigRef(configRaw)
	if err != nil {
		return CommonRecord{}, err
	}
	profilesRaw, err := require(object, "profile_refs")
	if err != nil {
		return CommonRecord{}, err
	}
	profiles, err := validateProfileRefs(profilesRaw)
	if err != nil {
		return CommonRecord{}, err
	}
	if err := validateRecordID(recordID, deviceID, "record_id"); err != nil || recordID != deviceID+"/"+bootID+"/"+sequence {
		return CommonRecord{}, contractError(InvalidIdentity, "record_id", "record_id does not match device_id/boot_id/sequence")
	}
	return CommonRecord{RecordID: recordID, RecordHash: recordHash, Kind: kind, DeviceID: deviceID, BootID: bootID, Sequence: sequence, MonotonicUS: monotonic, RecordedAt: recordedAt, TimeQuality: timeQuality, ConfigRef: configRef, ProfileRefs: profiles, RawObject: object}, nil
}

func validateRecordHash(raw []byte, object map[string]json.RawMessage, common CommonRecord) error {
	expected, _, err := RecordHash(raw)
	if err != nil {
		return err
	}
	if expected != common.RecordHash {
		return contractError(HashMismatch, "record_hash", "record hash does not match canonical record")
	}
	return nil
}

func validateSample(raw []byte, expectedDevice string) (SampleRecord, error) {
	if len(raw) > MaxRecordBytes {
		return SampleRecord{}, contractError(RecordTooLarge, "", "record exceeds 16 KiB")
	}
	object, err := strictJSON(raw, MaxRecordBytes)
	if err != nil {
		return SampleRecord{}, err
	}
	if err := requireFields(object, sampleFields); err != nil {
		return SampleRecord{}, err
	}
	common, err := validateCommon(object, "sample")
	if err != nil {
		return SampleRecord{}, err
	}
	if common.DeviceID != expectedDevice {
		return SampleRecord{}, contractError(InvalidIdentity, "device_id", "record device differs from batch device")
	}
	if err := validateRecordHash(raw, object, common); err != nil {
		return SampleRecord{}, err
	}
	sensor, err := decodeString(object, "sensor_id")
	if err != nil || !knownSensors[sensor] {
		return SampleRecord{}, contractError(InvalidEnum, "sensor_id", "unknown sensor")
	}
	stream, err := decodeString(object, "stream")
	if err != nil || !knownStreams[stream] {
		return SampleRecord{}, contractError(InvalidEnum, "stream", "unknown stream")
	}
	quality, err := decodeString(object, "quality")
	if err != nil || !knownQualities[quality] {
		return SampleRecord{}, contractError(InvalidEnum, "quality", "unknown sample quality")
	}
	reason, err := decodeString(object, "reason")
	if err != nil || !knownReasons[reason] {
		return SampleRecord{}, contractError(InvalidEnum, "reason", "unknown sample reason")
	}
	rawValue, err := require(object, "raw")
	if err != nil {
		return SampleRecord{}, err
	}
	converted, err := require(object, "converted")
	if err != nil {
		return SampleRecord{}, err
	}
	return SampleRecord{CommonRecord: common, SensorID: sensor, Stream: stream, Quality: quality, Reason: reason, Raw: rawValue, Converted: converted}, nil
}

func validateResult(raw []byte, expectedDevice string) (ResultRecord, error) {
	if len(raw) > MaxRecordBytes {
		return ResultRecord{}, contractError(RecordTooLarge, "", "record exceeds 16 KiB")
	}
	object, err := strictJSON(raw, MaxRecordBytes)
	if err != nil {
		return ResultRecord{}, err
	}
	if err := requireFields(object, resultFields); err != nil {
		return ResultRecord{}, err
	}
	common, err := validateCommon(object, "result")
	if err != nil {
		return ResultRecord{}, err
	}
	if common.DeviceID != expectedDevice {
		return ResultRecord{}, contractError(InvalidIdentity, "device_id", "record device differs from batch device")
	}
	if err := validateRecordHash(raw, object, common); err != nil {
		return ResultRecord{}, err
	}
	stream, err := decodeString(object, "stream")
	if err != nil || !knownStreams[stream] {
		return ResultRecord{}, contractError(InvalidEnum, "stream", "unknown stream")
	}
	computed, err := decodeUint64String(object, "computed_monotonic_us")
	if err != nil {
		return ResultRecord{}, err
	}
	var sourceIDs []string
	if err := json.Unmarshal(object["source_sample_ids"], &sourceIDs); err != nil || len(sourceIDs) == 0 {
		return ResultRecord{}, contractError(SourceIDsMissing, "source_sample_ids", "result must identify every source sample")
	}
	for index, sourceID := range sourceIDs {
		if err := validateRecordID(sourceID, expectedDevice, fmt.Sprintf("source_sample_ids[%d]", index)); err != nil {
			return ResultRecord{}, err
		}
	}
	processed, err := require(object, "processed")
	if err != nil {
		return ResultRecord{}, err
	}
	fuzzy, err := require(object, "fuzzy")
	if err != nil {
		return ResultRecord{}, err
	}
	safety, err := require(object, "safety")
	if err != nil {
		return ResultRecord{}, err
	}
	if err := validateUnavailableModels(fuzzy, "fuzzy"); err != nil {
		return ResultRecord{}, err
	}
	if err := validateUnavailableModels(safety, "safety"); err != nil {
		return ResultRecord{}, err
	}
	return ResultRecord{CommonRecord: common, Stream: stream, ComputedMonotonicUS: computed, SourceSampleIDs: sourceIDs, Processed: processed, Fuzzy: fuzzy, Safety: safety}, nil
}

func validateUnavailableModels(raw json.RawMessage, path string) error {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(raw, &object); err != nil || object == nil {
		return contractError(MalformedJSON, path, "expected object")
	}
	if err := requireFields(object, map[string]bool{"state": true, "score": true, "reason": true, "model_state": true, "trace": true}); err != nil {
		return err
	}
	stateRaw, ok := object["state"]
	if !ok {
		return contractError(MalformedJSON, path+".state", "state is required")
	}
	var state string
	if err := json.Unmarshal(stateRaw, &state); err != nil {
		return contractError(MalformedJSON, path+".state", err.Error())
	}
	if state == "UNAVAILABLE" {
		score, ok := object["score"]
		if !ok || !bytes.Equal(bytes.TrimSpace(score), []byte("null")) {
			return contractError(MalformedJSON, path+".score", "unavailable model score must be null")
		}
	}
	if state != "UNAVAILABLE" && state != "UNCONFIGURED" && state != "READY" && state != "DEGRADED" && state != "SAFE" {
		return contractError(InvalidEnum, path+".state", "unknown model state")
	}
	return nil
}

func validateEvent(raw []byte, expectedDevice string) (EventRecord, error) {
	object, err := strictJSON(raw, MaxRecordBytes)
	if err != nil {
		return EventRecord{}, err
	}
	if err := requireFields(object, eventFields); err != nil {
		return EventRecord{}, err
	}
	common, err := validateCommon(object, "event")
	if err != nil {
		return EventRecord{}, err
	}
	if common.DeviceID != expectedDevice {
		return EventRecord{}, contractError(InvalidIdentity, "device_id", "record device differs from batch device")
	}
	if err := validateRecordHash(raw, object, common); err != nil {
		return EventRecord{}, err
	}
	eventType, err := decodeString(object, "event_type")
	if err != nil || !knownEventTypes[eventType] {
		return EventRecord{}, contractError(InvalidEnum, "event_type", "unknown event type")
	}
	payload, err := require(object, "payload")
	if err != nil {
		return EventRecord{}, err
	}
	return EventRecord{CommonRecord: common, EventType: eventType, Payload: payload}, nil
}

func validateMetadata(raw []byte, expectedDevice string) (MetadataRecord, error) {
	object, err := strictJSON(raw, MaxRecordBytes)
	if err != nil {
		return MetadataRecord{}, err
	}
	if err := requireFields(object, metadataFields); err != nil {
		return MetadataRecord{}, err
	}
	common, err := validateCommon(object, "metadata")
	if err != nil {
		return MetadataRecord{}, err
	}
	if common.DeviceID != expectedDevice {
		return MetadataRecord{}, contractError(InvalidIdentity, "device_id", "record device differs from batch device")
	}
	if err := validateRecordHash(raw, object, common); err != nil {
		return MetadataRecord{}, err
	}
	metadataType, err := decodeString(object, "metadata_type")
	if err != nil || !knownMetadataTypes[metadataType] {
		return MetadataRecord{}, contractError(InvalidEnum, "metadata_type", "unknown metadata type")
	}
	payload, err := require(object, "payload")
	if err != nil {
		return MetadataRecord{}, err
	}
	return MetadataRecord{CommonRecord: common, MetadataType: metadataType, Payload: payload}, nil
}

// ValidateBatch performs all pure v3 boundary checks without persistence or inference.
func ValidateBatch(body []byte) (*Batch, error) {
	root, err := strictJSON(body, MaxBodyBytes)
	if err != nil {
		return nil, err
	}
	if err := requireFields(root, allowedBatchFields); err != nil {
		return nil, err
	}
	schema, err := decodeUint32(root, "schema_version")
	if err != nil || schema != 3 {
		return nil, contractError(SchemaUnsupported, "schema_version", "telemetry schema version must be 3")
	}
	deviceID, err := decodeString(root, "device_id")
	if err != nil {
		return nil, err
	}
	if err := validateID(deviceID, "device_id"); err != nil {
		return nil, err
	}
	batchID, err := decodeString(root, "batch_id")
	if err != nil {
		return nil, err
	}
	if !strings.HasPrefix(batchID, deviceID+"/") {
		return nil, contractError(InvalidIdentity, "batch_id", "batch ID must be owned by device")
	}
	countRaw, err := require(root, "record_count")
	if err != nil {
		return nil, err
	}
	var count uint16
	if err := json.Unmarshal(countRaw, &count); err != nil || count > MaxRecords {
		return nil, contractError(CountMismatch, "record_count", "record count is outside the frozen limit")
	}
	decodeArray := func(key string) ([]json.RawMessage, error) {
		raw, err := require(root, key)
		if err != nil {
			return nil, err
		}
		var values []json.RawMessage
		if err := json.Unmarshal(raw, &values); err != nil || values == nil {
			return nil, contractError(MalformedJSON, key, "expected array")
		}
		return values, nil
	}
	samples, err := decodeArray("samples")
	if err != nil {
		return nil, err
	}
	results, err := decodeArray("results")
	if err != nil {
		return nil, err
	}
	events, err := decodeArray("events")
	if err != nil {
		return nil, err
	}
	metadata, err := decodeArray("metadata")
	if err != nil {
		return nil, err
	}
	if len(samples)+len(results)+len(events)+len(metadata) != int(count) {
		return nil, contractError(CountMismatch, "record_count", "record_count does not equal array totals")
	}
	batch := &Batch{SchemaVersion: schema, DeviceID: deviceID, BatchID: batchID, RecordCount: count}
	seen := make(map[string]string, int(count))
	checkUnique := func(common CommonRecord) error {
		if previous, ok := seen[common.RecordID]; ok {
			if previous == common.RecordHash {
				return contractError(DuplicateRecordID, "record_id", "record ID appears more than once")
			}
			return contractError(Conflict, "record_id", "record ID has conflicting hashes")
		}
		seen[common.RecordID] = common.RecordHash
		return nil
	}
	for _, raw := range samples {
		record, err := validateSample(raw, deviceID)
		if err != nil {
			return nil, err
		}
		if err := checkUnique(record.CommonRecord); err != nil {
			return nil, err
		}
		batch.Samples = append(batch.Samples, record)
	}
	for _, raw := range results {
		record, err := validateResult(raw, deviceID)
		if err != nil {
			return nil, err
		}
		if err := checkUnique(record.CommonRecord); err != nil {
			return nil, err
		}
		batch.Results = append(batch.Results, record)
	}
	for _, raw := range events {
		record, err := validateEvent(raw, deviceID)
		if err != nil {
			return nil, err
		}
		if err := checkUnique(record.CommonRecord); err != nil {
			return nil, err
		}
		batch.Events = append(batch.Events, record)
	}
	for _, raw := range metadata {
		record, err := validateMetadata(raw, deviceID)
		if err != nil {
			return nil, err
		}
		if err := checkUnique(record.CommonRecord); err != nil {
			return nil, err
		}
		batch.Metadata = append(batch.Metadata, record)
	}
	return batch, nil
}
