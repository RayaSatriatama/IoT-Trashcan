package telemetry

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
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

// sensorStreams is intentionally explicit. Sensor identity and logical stream
// are one contract: accepting them independently would allow records from one
// physical channel to be attributed to another.
var sensorStreams = map[string]string{
	"mq4":                  "organic",
	"mq135":                "organic",
	"mq7":                  "b3",
	"mq8":                  "b3",
	"hx711_organic":        "organic",
	"hx711_inorganic":      "inorganic",
	"hx711_b3":             "b3",
	"ultrasonic_organic":   "organic",
	"ultrasonic_inorganic": "inorganic",
	"ultrasonic_b3":        "b3",
	"ds18b20_b3":           "b3",
	"sht31_44":             "organic",
	"sht31_45":             "b3",
}

var knownStreams = map[string]bool{"organic": true, "inorganic": true, "b3": true, "ambient": true}
var knownResultStreams = map[string]bool{"organic": true, "inorganic": true, "b3": true}
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

func decodePayloadObject(raw json.RawMessage, path string) (map[string]json.RawMessage, error) {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(raw, &object); err != nil || object == nil {
		return nil, contractError(MalformedJSON, path, "expected object")
	}
	return object, nil
}

func decodeInt64Value(raw json.RawMessage, path string) (int64, error) {
	var value int64
	if err := json.Unmarshal(raw, &value); err != nil {
		return 0, contractError(MalformedJSON, path, "expected integer")
	}
	return value, nil
}

func decodeUint32Value(raw json.RawMessage, path string) (uint32, error) {
	var value uint32
	if err := json.Unmarshal(raw, &value); err != nil {
		return 0, contractError(MalformedJSON, path, "expected non-negative integer")
	}
	return value, nil
}

func decodeFiniteNumber(raw json.RawMessage, path string) (float64, error) {
	var value float64
	if err := json.Unmarshal(raw, &value); err != nil || math.IsNaN(value) || math.IsInf(value, 0) {
		return 0, contractError(NonFiniteNumber, path, "expected finite JSON number")
	}
	return value, nil
}

func requireNullOrPayloadObject(raw json.RawMessage, path string) (map[string]json.RawMessage, bool, error) {
	if bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return nil, true, nil
	}
	object, err := decodePayloadObject(raw, path)
	return object, false, err
}

func isJSONNull(raw json.RawMessage) bool {
	return bytes.Equal(bytes.TrimSpace(raw), []byte("null"))
}

// A failed acquisition still has a sample envelope, but must not claim a
// converted value when no trustworthy raw evidence reached the device. The
// sensor-specific validators call this before decoding typed payloads.
func validateAbsentRaw(raw, converted json.RawMessage) error {
	if !isJSONNull(raw) {
		return contractError(MalformedJSON, "raw", "raw evidence must be null when no evidence was received")
	}
	if !isJSONNull(converted) {
		return contractError(MalformedJSON, "converted", "converted value requires raw evidence")
	}
	return nil
}

func validateSensorStream(sensorID, stream string) error {
	expected, ok := sensorStreams[sensorID]
	if !ok {
		return contractError(InvalidEnum, "sensor_id", "unknown sensor")
	}
	if stream != expected {
		return contractError(InvalidIdentity, "stream", "sensor does not belong to stream")
	}
	return nil
}

func validateHX711Payload(raw, converted json.RawMessage) error {
	if isJSONNull(raw) {
		return validateAbsentRaw(raw, converted)
	}
	rawObject, err := decodePayloadObject(raw, "raw")
	if err != nil {
		return err
	}
	if err := requireFields(rawObject, map[string]bool{"counts": true}); err != nil {
		return err
	}
	if _, err := decodeInt64Value(rawObject["counts"], "raw.counts"); err != nil {
		return err
	}
	object, isNull, err := requireNullOrPayloadObject(converted, "converted")
	if err != nil || isNull {
		return err
	}
	if err := requireFields(object, map[string]bool{"kg": true}); err != nil {
		return err
	}
	_, err = decodeFiniteNumber(object["kg"], "converted.kg")
	return err
}

func validateUltrasonicPayload(raw, converted json.RawMessage) error {
	if isJSONNull(raw) {
		return validateAbsentRaw(raw, converted)
	}
	rawObject, err := decodePayloadObject(raw, "raw")
	if err != nil {
		return err
	}
	if err := requireFields(rawObject, map[string]bool{"echo_us": true}); err != nil {
		return err
	}
	if _, err := decodeUint32Value(rawObject["echo_us"], "raw.echo_us"); err != nil {
		return err
	}
	object, isNull, err := requireNullOrPayloadObject(converted, "converted")
	if err != nil || isNull {
		return err
	}
	if err := requireFields(object, map[string]bool{"distance_mm": true, "fill_percent": true}); err != nil {
		return err
	}
	for _, key := range []string{"distance_mm", "fill_percent"} {
		if bytes.Equal(bytes.TrimSpace(object[key]), []byte("null")) {
			continue
		}
		if _, err := decodeFiniteNumber(object[key], "converted."+key); err != nil {
			return err
		}
	}
	return nil
}

var mqHeaterPhases = map[string]bool{
	"PREHEAT": true, "HIGH_CLEAN": true, "LOW_DETECT": true, "FAULT": true,
}

func validateMQPayload(sensorID, quality string, raw, converted json.RawMessage) error {
	if isJSONNull(raw) {
		return validateAbsentRaw(raw, converted)
	}
	rawObject, err := decodePayloadObject(raw, "raw")
	if err != nil {
		return err
	}
	allowed := map[string]bool{"adc_counts": true}
	if sensorID == "mq7" {
		allowed["heater_phase"] = true
	}
	if err := requireFields(rawObject, allowed); err != nil {
		return err
	}
	if _, err := decodeUint32Value(rawObject["adc_counts"], "raw.adc_counts"); err != nil {
		return err
	}
	if sensorID == "mq7" {
		if _, ok := rawObject["heater_phase"]; !ok {
			return contractError(MalformedJSON, "raw.heater_phase", "MQ-7 heater phase is required")
		}
	}
	if phase, ok := rawObject["heater_phase"]; ok {
		var value string
		if err := json.Unmarshal(phase, &value); err != nil || !mqHeaterPhases[value] {
			return contractError(InvalidEnum, "raw.heater_phase", "unknown MQ-7 heater phase")
		}
	}
	object, isNull, err := requireNullOrPayloadObject(converted, "converted")
	if err != nil || isNull {
		return err
	}
	allowedConverted := map[string]bool{"adc_mv": true}
	if sensorID == "mq135" {
		allowedConverted["adc_pin_mv"] = true
		allowedConverted["sensor_ao_mv"] = true
		allowedConverted["adc_efuse_calibrated"] = true
	}
	if err := requireFields(object, allowedConverted); err != nil {
		return err
	}
	adcRaw, ok := object["adc_mv"]
	if !ok {
		return contractError(MalformedJSON, "converted.adc_mv", "required MQ voltage is missing")
	}
	if bytes.Equal(bytes.TrimSpace(adcRaw), []byte("null")) {
		for _, key := range []string{"adc_pin_mv", "sensor_ao_mv", "adc_efuse_calibrated"} {
			if _, ok := object[key]; ok {
				return contractError(MalformedJSON, "converted."+key, "calibrated voltage fields require adc_mv")
			}
		}
		return nil
	}
	adcMV, err := decodeFiniteNumber(adcRaw, "converted.adc_mv")
	if err != nil {
		return err
	}
	if sensorID != "mq135" {
		return nil
	}
	calibratedKeys := []string{"adc_pin_mv", "sensor_ao_mv", "adc_efuse_calibrated"}
	present := 0
	for _, key := range calibratedKeys {
		if _, ok := object[key]; ok {
			present++
		}
	}
	if present == 0 {
		return nil
	}
	if present != len(calibratedKeys) {
		return contractError(MalformedJSON, "converted", "calibrated MQ135 voltage fields must be supplied together")
	}
	if quality != "READY" {
		return contractError(InvalidIdentity, "quality", "calibrated MQ135 voltage requires READY quality")
	}
	if _, err := decodeFiniteNumber(object["adc_pin_mv"], "converted.adc_pin_mv"); err != nil {
		return err
	}
	sensorAOMV, err := decodeFiniteNumber(object["sensor_ao_mv"], "converted.sensor_ao_mv")
	if err != nil {
		return err
	}
	if sensorAOMV != adcMV {
		return contractError(InvalidIdentity, "converted.sensor_ao_mv", "sensor_ao_mv must preserve the adc_mv legacy alias")
	}
	calibrated, err := decodeFiniteNumber(object["adc_efuse_calibrated"], "converted.adc_efuse_calibrated")
	if err != nil {
		return err
	}
	if calibrated != 1 {
		return contractError(InvalidIdentity, "converted.adc_efuse_calibrated", "calibration marker must equal 1")
	}
	return nil
}

func validateDS18B20Payload(raw, converted json.RawMessage) error {
	if isJSONNull(raw) {
		return validateAbsentRaw(raw, converted)
	}
	rawObject, err := decodePayloadObject(raw, "raw")
	if err != nil {
		return err
	}
	allowed := map[string]bool{"scratchpad_hex": true, "rom": true, "crc_valid": true, "conversion_complete": true, "resolution_bits": true}
	if err := requireFields(rawObject, allowed); err != nil {
		return err
	}
	for _, key := range []string{"rom", "scratchpad_hex", "crc_valid", "conversion_complete", "resolution_bits"} {
		if _, ok := rawObject[key]; !ok {
			return contractError(MalformedJSON, "raw."+key, "DS18B20 raw evidence is required")
		}
	}
	scratchpad, err := decodeString(rawObject, "scratchpad_hex")
	if err != nil || !regexp.MustCompile(`^[0-9a-fA-F]{18}$`).MatchString(scratchpad) {
		return contractError(MalformedJSON, "raw.scratchpad_hex", "expected complete 9-byte scratchpad hex")
	}
	rom, err := decodeString(map[string]json.RawMessage{"rom": rawObject["rom"]}, "rom")
	if err != nil || !regexp.MustCompile(`^[0-9a-fA-F]{16}$`).MatchString(rom) {
		return contractError(MalformedJSON, "raw.rom", "expected 8-byte ROM hex")
	}
	romBytes, err := hex.DecodeString(rom)
	if err != nil || len(romBytes) != 8 || romBytes[0] != 0x28 || ds18b20CRC8(romBytes[:7]) != romBytes[7] {
		return contractError(MalformedJSON, "raw.rom", "ROM identity has an invalid DS18B20 CRC")
	}
	scratchpadBytes, err := hex.DecodeString(scratchpad)
	if err != nil || len(scratchpadBytes) != 9 {
		return contractError(MalformedJSON, "raw.scratchpad_hex", "expected complete 9-byte scratchpad hex")
	}
	var crcValid bool
	if err := json.Unmarshal(rawObject["crc_valid"], &crcValid); err != nil {
		return contractError(MalformedJSON, "raw.crc_valid", "expected boolean")
	}
	if crcValid && ds18b20CRC8(scratchpadBytes[:8]) != scratchpadBytes[8] {
		return contractError(MalformedJSON, "raw.scratchpad_hex", "crc_valid evidence does not match scratchpad CRC")
	}
	var conversionComplete bool
	if err := json.Unmarshal(rawObject["conversion_complete"], &conversionComplete); err != nil {
		return contractError(MalformedJSON, "raw.conversion_complete", "expected boolean")
	}
	_ = conversionComplete
	resolution, err := decodeUint32Value(rawObject["resolution_bits"], "raw.resolution_bits")
	if err != nil || resolution < 9 || resolution > 12 {
		return contractError(InvalidEnum, "raw.resolution_bits", "resolution must be 9 through 12")
	}
	object, isNull, err := requireNullOrPayloadObject(converted, "converted")
	if err != nil || isNull {
		return err
	}
	if err := requireFields(object, map[string]bool{"temperature_c": true}); err != nil {
		return err
	}
	if bytes.Equal(bytes.TrimSpace(object["temperature_c"]), []byte("null")) {
		return nil
	}
	_, err = decodeFiniteNumber(object["temperature_c"], "converted.temperature_c")
	return err
}

func ds18b20CRC8(data []byte) byte {
	crc := byte(0)
	for _, value := range data {
		for bit := byte(0); bit < 8; bit++ {
			mix := (crc ^ value) & 1
			crc >>= 1
			if mix != 0 {
				crc ^= 0x8c
			}
			value >>= 1
		}
	}
	return crc
}

func validateSHT31Payload(raw, converted json.RawMessage) error {
	if isJSONNull(raw) {
		return validateAbsentRaw(raw, converted)
	}
	rawObject, err := decodePayloadObject(raw, "raw")
	if err != nil {
		return err
	}
	allowed := map[string]bool{"temperature_ticks": true, "humidity_ticks": true, "temperature_crc_valid": true, "humidity_crc_valid": true}
	if err := requireFields(rawObject, allowed); err != nil {
		return err
	}
	for _, key := range []string{"temperature_ticks", "humidity_ticks"} {
		if _, err := decodeUint32Value(rawObject[key], "raw."+key); err != nil {
			return err
		}
	}
	for _, key := range []string{"temperature_crc_valid", "humidity_crc_valid"} {
		var flag bool
		if err := json.Unmarshal(rawObject[key], &flag); err != nil {
			return contractError(MalformedJSON, "raw."+key, "expected boolean")
		}
	}
	object, isNull, err := requireNullOrPayloadObject(converted, "converted")
	if err != nil || isNull {
		return err
	}
	if err := requireFields(object, map[string]bool{"temperature_c": true, "humidity_percent": true}); err != nil {
		return err
	}
	for _, key := range []string{"temperature_c", "humidity_percent"} {
		value := object[key]
		if bytes.Equal(bytes.TrimSpace(value), []byte("null")) {
			continue
		}
		if _, err := decodeFiniteNumber(value, "converted."+key); err != nil {
			return err
		}
	}
	return nil
}

func validateSamplePayload(sensorID, stream, quality, reason string, raw, converted json.RawMessage) error {
	if err := validateSensorStream(sensorID, stream); err != nil {
		return err
	}
	if quality == "READY" && isJSONNull(raw) {
		return contractError(MalformedJSON, "raw", "READY samples require raw evidence")
	}
	if quality == "READY" && (reason == "TIMEOUT" || reason == "DISCONNECTED" || reason == "CRC_ERROR") {
		return contractError(InvalidIdentity, "reason", "READY samples cannot carry acquisition-failure reasons")
	}
	if quality == "TIMEOUT" && reason == "DATA_READY" {
		return contractError(InvalidIdentity, "reason", "TIMEOUT samples cannot claim DATA_READY")
	}
	if quality == "DISCONNECTED" && reason != "DISCONNECTED" {
		return contractError(InvalidIdentity, "reason", "DISCONNECTED samples require DISCONNECTED reason")
	}
	if quality == "CRC_ERROR" && reason != "CRC_ERROR" {
		return contractError(InvalidIdentity, "reason", "CRC_ERROR samples require CRC_ERROR reason")
	}
	if (quality == "TIMEOUT" || quality == "DISCONNECTED" || quality == "CRC_ERROR") && !isJSONNull(converted) {
		return contractError(MalformedJSON, "converted", "acquisition failures cannot carry an operational value")
	}
	switch sensorID {
	case "hx711_organic", "hx711_inorganic", "hx711_b3":
		return validateHX711Payload(raw, converted)
	case "ultrasonic_organic", "ultrasonic_inorganic", "ultrasonic_b3":
		return validateUltrasonicPayload(raw, converted)
	case "mq4", "mq135", "mq7", "mq8":
		return validateMQPayload(sensorID, quality, raw, converted)
	case "ds18b20_b3":
		return validateDS18B20Payload(raw, converted)
	case "sht31_44", "sht31_45":
		return validateSHT31Payload(raw, converted)
	default:
		return contractError(InvalidEnum, "sensor_id", "unknown sensor")
	}
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
	if len(objects) > MaxProfileRefsPerRecord {
		return nil, contractError(ProfileMalformed, "profile_refs", "profile reference count exceeds the per-record limit")
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
	if err := validateSamplePayload(sensor, stream, quality, reason, rawValue, converted); err != nil {
		return SampleRecord{}, err
	}
	if err := validateRecordHash(raw, object, common); err != nil {
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
	stream, err := decodeString(object, "stream")
	if err != nil || !knownResultStreams[stream] {
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
	if err := validateProcessed(processed, "processed"); err != nil {
		return ResultRecord{}, err
	}
	if err := validateFuzzyResult(fuzzy, "fuzzy"); err != nil {
		return ResultRecord{}, err
	}
	if err := validateSafetyResult(safety, "safety"); err != nil {
		return ResultRecord{}, err
	}
	if err := validateRecordHash(raw, object, common); err != nil {
		return ResultRecord{}, err
	}
	return ResultRecord{CommonRecord: common, Stream: stream, ComputedMonotonicUS: computed, SourceSampleIDs: sourceIDs, Processed: processed, Fuzzy: fuzzy, Safety: safety}, nil
}

var fuzzyStates = map[string]bool{
	"UNAVAILABLE": true, "UNCONFIGURED": true, "READY": true,
}

var modelModes = map[string]bool{
	"UNCONFIGURED": true, "TEST_ONLY": true, "PRODUCTION": true,
}

var fuzzyReasons = map[string]bool{
	"INPUT_MISSING": true, "MODEL_UNCONFIGURED": true, "MODEL_INVALID": true,
	"MODEL_NOT_CONFIGURED": true, "FIS_NOT_CONFIGURED": true,
	"ZERO_AREA": true, "READY": true, "STALE_INPUT": true,
	"CALIBRATING": true,
}

var safetyStates = map[string]bool{
	"UNCONFIGURED": true, "UNAVAILABLE": true, "FAULT": true,
	"CLEAR": true, "ALERT": true,
}

var safetyReasons = map[string]bool{
	"INPUT_MISSING": true, "SAFETY_NOT_CONFIGURED": true,
	"HAZARD_INPUT_INVALID": true, "ALERT_LATCHED": true,
	"ALL_VALIDATED_CHANNELS_CLEAR": true, "TEST_ONLY": true,
}

func decodeObject(raw json.RawMessage, path string) (map[string]json.RawMessage, error) {
	object, err := strictJSON(raw, MaxRecordBytes)
	if err != nil {
		return nil, err
	}
	if object == nil {
		return nil, contractError(MalformedJSON, path, "expected object")
	}
	return object, nil
}

func validateNullableNumber(raw json.RawMessage, path string) error {
	if bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return nil
	}
	var number float64
	if err := json.Unmarshal(raw, &number); err != nil {
		return contractError(MalformedJSON, path, "expected finite JSON number or null")
	}
	if math.IsNaN(number) || math.IsInf(number, 0) {
		return contractError(NonFiniteNumber, path, "number must be finite")
	}
	return nil
}

func validateProcessed(raw json.RawMessage, path string) error {
	object, err := decodeObject(raw, path)
	if err != nil {
		return err
	}
	allowed := map[string]bool{
		"fill_pct": true, "weight_kg": true, "weight_utilization_pct": true,
		"ogi": true, "accumulation_hours": true, "temperature_c": true,
	}
	if err := requireFields(object, allowed); err != nil {
		return err
	}
	for key := range allowed {
		if _, ok := object[key]; !ok {
			return contractError(MalformedJSON, path+"."+key, "processed field is required")
		}
	}
	for key, value := range object {
		if err := validateNullableNumber(value, path+"."+key); err != nil {
			return err
		}
	}
	return nil
}

func validateFuzzyResult(raw json.RawMessage, path string) error {
	object, err := decodeObject(raw, path)
	if err != nil {
		return err
	}
	if err := requireFields(object, map[string]bool{"state": true, "score": true, "label": true, "reason": true, "model_state": true, "trace": true}); err != nil {
		return err
	}
	state, err := decodeString(object, "state")
	if err != nil || !fuzzyStates[state] {
		return contractError(InvalidEnum, path+".state", "unknown model state")
	}
	if err := validateNullableNumber(object["score"], path+".score"); err != nil {
		return err
	}
	if bytes.Equal(bytes.TrimSpace(object["label"]), []byte("null")) {
		if state == "READY" {
			return contractError(MalformedJSON, path+".label", "ready fuzzy result requires a label")
		}
	} else {
		label, err := decodeString(object, "label")
		if err != nil || len(label) == 0 || len(label) > 128 {
			return contractError(MalformedJSON, path+".label", "label must be a bounded string or null")
		}
	}
	if (state == "UNAVAILABLE" || state == "UNCONFIGURED") && !bytes.Equal(bytes.TrimSpace(object["score"]), []byte("null")) {
		return contractError(MalformedJSON, path+".score", "unavailable model score must be null")
	}
	if state == "READY" && bytes.Equal(bytes.TrimSpace(object["score"]), []byte("null")) {
		return contractError(MalformedJSON, path+".score", "operational model score is required")
	}
	if state == "READY" && bytes.Equal(bytes.TrimSpace(object["trace"]), []byte("null")) {
		return contractError(MalformedJSON, path+".trace", "ready fuzzy result requires a trace")
	}
	reason, err := decodeString(object, "reason")
	if err != nil || !fuzzyReasons[reason] {
		return contractError(InvalidEnum, path+".reason", "unknown model reason")
	}
	mode, err := decodeString(object, "model_state")
	if err != nil || !modelModes[mode] {
		return contractError(InvalidEnum, path+".model_state", "unknown model state provenance")
	}
	if err := validateTrace(object["trace"], path+".trace"); err != nil {
		return err
	}
	return validateFuzzyTraceOutput(object["trace"], path+".trace")
}

func validateFuzzyTraceOutput(raw json.RawMessage, path string) error {
	if bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return nil
	}
	object, err := decodeObject(raw, path)
	if err != nil {
		return err
	}
	aggregate, ok := object["aggregate"]
	if !ok || bytes.Equal(bytes.TrimSpace(aggregate), []byte("null")) {
		return contractError(MalformedJSON, path+".aggregate", "fuzzy trace requires aggregate output evidence")
	}
	aggregateObject, err := decodeObject(aggregate, path+".aggregate")
	if err != nil {
		return err
	}
	if _, ok := aggregateObject["output_id"]; !ok {
		return contractError(MalformedJSON, path+".aggregate.output_id", "fuzzy aggregate output identity is required")
	}
	outputID, err := decodeString(aggregateObject, "output_id")
	if err != nil || len(outputID) == 0 || len(outputID) > 128 {
		return contractError(MalformedJSON, path+".aggregate.output_id", "fuzzy aggregate output identity must be a bounded string")
	}
	return nil
}

func validateSafetyResult(raw json.RawMessage, path string) error {
	object, err := decodeObject(raw, path)
	if err != nil {
		return err
	}
	if err := requireFields(object, map[string]bool{"state": true, "reason": true, "model_state": true, "trace": true}); err != nil {
		return err
	}
	state, err := decodeString(object, "state")
	if err != nil || !safetyStates[state] {
		return contractError(InvalidEnum, path+".state", "unknown safety state")
	}
	reason, err := decodeString(object, "reason")
	if err != nil || !safetyReasons[reason] {
		return contractError(InvalidEnum, path+".reason", "unknown safety reason")
	}
	if state == "CLEAR" && reason != "ALL_VALIDATED_CHANNELS_CLEAR" {
		return contractError(InvalidIdentity, path+".state", "CLEAR requires validated clear evidence")
	}
	mode, err := decodeString(object, "model_state")
	if err != nil || !modelModes[mode] {
		return contractError(InvalidEnum, path+".model_state", "unknown safety model provenance")
	}
	if (state == "CLEAR" || state == "ALERT") && mode == "UNCONFIGURED" {
		return contractError(InvalidIdentity, path+".model_state", "operational safety states require a configured model")
	}
	if state == "UNCONFIGURED" && mode != "UNCONFIGURED" {
		return contractError(InvalidIdentity, path+".model_state", "UNCONFIGURED safety state requires unconfigured model provenance")
	}
	return validateTrace(object["trace"], path+".trace")
}

func validateTrace(raw json.RawMessage, path string) error {
	if bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return nil
	}
	object, err := decodeObject(raw, path)
	if err != nil {
		return err
	}
	if err := requireFields(object, map[string]bool{"model_ref": true, "memberships": true, "rules": true, "aggregate": true}); err != nil {
		return err
	}
	modelRef, err := decodeObject(object["model_ref"], path+".model_ref")
	if err != nil {
		return err
	}
	if err := requireFields(modelRef, map[string]bool{"version": true, "hash": true}); err != nil {
		return err
	}
	if _, err := decodeUint64String(modelRef, "version"); err != nil {
		return err
	}
	hash, err := decodeString(modelRef, "hash")
	if err != nil {
		return err
	}
	if err := validateHash(hash, path+".model_ref.hash"); err != nil {
		return err
	}
	for _, key := range []string{"memberships", "rules"} {
		var values []json.RawMessage
		if err := json.Unmarshal(object[key], &values); err != nil || values == nil {
			return contractError(MalformedJSON, path+"."+key, "expected array")
		}
		for index, value := range values {
			entry, err := decodeObject(value, fmt.Sprintf("%s.%s[%d]", path, key, index))
			if err != nil {
				return err
			}
			if key == "memberships" {
				if err := requireFields(entry, map[string]bool{"input": true, "set": true, "value": true}); err != nil {
					return err
				}
				if _, err := decodeString(entry, "input"); err != nil {
					return err
				}
				if _, err := decodeString(entry, "set"); err != nil {
					return err
				}
				if err := validateNullableNumber(entry["value"], fmt.Sprintf("%s.memberships[%d].value", path, index)); err != nil || bytes.Equal(bytes.TrimSpace(entry["value"]), []byte("null")) {
					if err != nil {
						return err
					}
					return contractError(MalformedJSON, fmt.Sprintf("%s.memberships[%d].value", path, index), "membership value must be a number")
				}
			} else {
				if err := requireFields(entry, map[string]bool{"id": true, "strength": true, "output_id": true}); err != nil {
					return err
				}
				for _, required := range []string{"id", "strength", "output_id"} {
					if _, ok := entry[required]; !ok {
						return contractError(MalformedJSON, fmt.Sprintf("%s.rules[%d].%s", path, index, required), "rule evidence field is required")
					}
				}
				if _, err := decodeString(entry, "id"); err != nil {
					return err
				}
				outputID, err := decodeString(entry, "output_id")
				if err != nil || len(outputID) == 0 || len(outputID) > 128 {
					return contractError(MalformedJSON, fmt.Sprintf("%s.rules[%d].output_id", path, index), "rule output identity must be a bounded string")
				}
				if err := validateNullableNumber(entry["strength"], fmt.Sprintf("%s.rules[%d].strength", path, index)); err != nil || bytes.Equal(bytes.TrimSpace(entry["strength"]), []byte("null")) {
					if err != nil {
						return err
					}
					return contractError(MalformedJSON, fmt.Sprintf("%s.rules[%d].strength", path, index), "rule strength must be a number")
				}
			}
		}
	}
	aggregate := object["aggregate"]
	if bytes.Equal(bytes.TrimSpace(aggregate), []byte("null")) {
		return nil
	}
	aggregateObject, err := decodeObject(aggregate, path+".aggregate")
	if err != nil {
		return err
	}
	if err := requireFields(aggregateObject, map[string]bool{"output_id": true, "area": true, "centroid": true}); err != nil {
		return err
	}
	outputID, err := decodeString(aggregateObject, "output_id")
	if err != nil || len(outputID) == 0 || len(outputID) > 128 {
		return contractError(MalformedJSON, path+".aggregate.output_id", "aggregate output identity must be a bounded string")
	}
	if err := validateNullableNumber(aggregateObject["area"], path+".aggregate.area"); err != nil {
		return err
	}
	return validateNullableNumber(aggregateObject["centroid"], path+".aggregate.centroid")
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
	eventType, err := decodeString(object, "event_type")
	if err != nil || !knownEventTypes[eventType] {
		return EventRecord{}, contractError(InvalidEnum, "event_type", "unknown event type")
	}
	payload, err := require(object, "payload")
	if err != nil {
		return EventRecord{}, err
	}
	if err := validateEventPayload(eventType, payload); err != nil {
		return EventRecord{}, err
	}
	if err := validateRecordHash(raw, object, common); err != nil {
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
	metadataType, err := decodeString(object, "metadata_type")
	if err != nil || !knownMetadataTypes[metadataType] {
		return MetadataRecord{}, contractError(InvalidEnum, "metadata_type", "unknown metadata type")
	}
	payload, err := require(object, "payload")
	if err != nil {
		return MetadataRecord{}, err
	}
	if err := validateMetadataPayload(metadataType, payload); err != nil {
		return MetadataRecord{}, err
	}
	if err := validateRecordHash(raw, object, common); err != nil {
		return MetadataRecord{}, err
	}
	return MetadataRecord{CommonRecord: common, MetadataType: metadataType, Payload: payload}, nil
}

var clockSources = map[string]bool{"NTP": true, "SNTP": true, "RTC": true, "MANUAL": true, "GATEWAY": true}
var gapCauses = map[string]bool{"QUEUE_FULL": true, "STORAGE_ERROR": true, "POWER_LOSS": true, "SENSOR_ERROR": true, "UNKNOWN": true}
var resetReasons = map[string]bool{"POWER_ON": true, "BROWNOUT": true, "WATCHDOG": true, "SOFTWARE": true, "UNKNOWN": true}
var healthStates = map[string]bool{"ONLINE": true, "DEGRADED": true, "FAILED": true, "UNKNOWN": true}
var faultSeverities = map[string]bool{"INFO": true, "WARNING": true, "ERROR": true, "FATAL": true}
var activationReasons = map[string]bool{"BOOT": true, "COMMIT": true, "RECOVERY": true, "FACTORY_DEFAULT": true}

func validateEventPayload(eventType string, raw json.RawMessage) error {
	object, err := decodeObject(raw, "payload")
	if err != nil {
		return err
	}
	switch eventType {
	case "CLOCK_ANCHOR":
		if err := requireFields(object, map[string]bool{"anchor_monotonic_us": true, "utc": true, "uncertainty_us": true, "source": true}); err != nil {
			return err
		}
		if _, err := decodeUint64String(object, "anchor_monotonic_us"); err != nil {
			return err
		}
		if !bytes.Equal(bytes.TrimSpace(object["utc"]), []byte("null")) {
			utc, err := decodeString(object, "utc")
			if err != nil {
				return err
			}
			if _, err := time.Parse(time.RFC3339Nano, utc); err != nil {
				return contractError(MalformedJSON, "payload.utc", "expected RFC3339 timestamp or null")
			}
		}
		if _, err := decodeUint64String(object, "uncertainty_us"); err != nil {
			return err
		}
		source, err := decodeString(object, "source")
		if err != nil || !clockSources[source] {
			return contractError(InvalidEnum, "payload.source", "unknown clock source")
		}
	case "GAP":
		if err := requireFields(object, map[string]bool{"sequence_start": true, "sequence_end": true, "sensor_id": true, "stream": true, "reason": true, "cause": true}); err != nil {
			return err
		}
		start, err := decodeUint64String(object, "sequence_start")
		if err != nil {
			return err
		}
		end, err := decodeUint64String(object, "sequence_end")
		if err != nil {
			return err
		}
		startValue, _ := ParseUint64Decimal(start)
		endValue, _ := ParseUint64Decimal(end)
		if startValue > endValue {
			return contractError(InvalidIdentity, "payload.sequence_end", "gap sequence range is reversed")
		}
		var sensorID string
		var streamID string
		if !bytes.Equal(bytes.TrimSpace(object["sensor_id"]), []byte("null")) {
			sensor, err := decodeString(object, "sensor_id")
			if err != nil || !knownSensors[sensor] {
				return contractError(InvalidEnum, "payload.sensor_id", "unknown gap sensor")
			}
			sensorID = sensor
		}
		if !bytes.Equal(bytes.TrimSpace(object["stream"]), []byte("null")) {
			stream, err := decodeString(object, "stream")
			if err != nil || !knownStreams[stream] {
				return contractError(InvalidEnum, "payload.stream", "unknown gap stream")
			}
			streamID = stream
		}
		if sensorID != "" && streamID != "" {
			if err := validateSensorStream(sensorID, streamID); err != nil {
				return err
			}
		}
		if _, err := decodeString(object, "reason"); err != nil {
			return err
		}
		cause, err := decodeString(object, "cause")
		if err != nil || !gapCauses[cause] {
			return contractError(InvalidEnum, "payload.cause", "unknown gap cause")
		}
	case "RESET":
		if err := requireFields(object, map[string]bool{"reset_reason": true}); err != nil {
			return err
		}
		reason, err := decodeString(object, "reset_reason")
		if err != nil || !resetReasons[reason] {
			return contractError(InvalidEnum, "payload.reset_reason", "unknown reset reason")
		}
	case "HEALTH_TRANSITION":
		if err := requireFields(object, map[string]bool{"component": true, "from": true, "to": true, "reason": true}); err != nil {
			return err
		}
		if _, err := decodeString(object, "component"); err != nil {
			return err
		}
		from, err := decodeString(object, "from")
		if err != nil || !healthStates[from] {
			return contractError(InvalidEnum, "payload.from", "unknown previous health state")
		}
		to, err := decodeString(object, "to")
		if err != nil || !healthStates[to] || to == "UNKNOWN" {
			return contractError(InvalidEnum, "payload.to", "unknown next health state")
		}
		if _, err := decodeString(object, "reason"); err != nil {
			return err
		}
	case "FAULT":
		if err := requireFields(object, map[string]bool{"component": true, "code": true, "severity": true, "detail": true}); err != nil {
			return err
		}
		for _, key := range []string{"component", "code"} {
			if _, err := decodeString(object, key); err != nil {
				return err
			}
		}
		severity, err := decodeString(object, "severity")
		if err != nil || !faultSeverities[severity] {
			return contractError(InvalidEnum, "payload.severity", "unknown fault severity")
		}
		if !bytes.Equal(bytes.TrimSpace(object["detail"]), []byte("null")) {
			if _, err := decodeString(object, "detail"); err != nil {
				return err
			}
		}
	}
	return nil
}

func validateMetadataPayload(metadataType string, raw json.RawMessage) error {
	object, err := decodeObject(raw, "payload")
	if err != nil {
		return err
	}
	switch metadataType {
	case "CONFIG_ACTIVATION":
		if err := requireFields(object, map[string]bool{"config_ref": true, "previous_config_ref": true, "reason": true}); err != nil {
			return err
		}
		if _, err := validateConfigRef(object["config_ref"]); err != nil {
			return err
		}
		if !bytes.Equal(bytes.TrimSpace(object["previous_config_ref"]), []byte("null")) {
			if _, err := validateConfigRef(object["previous_config_ref"]); err != nil {
				return err
			}
		}
	case "PROFILE_ACTIVATION":
		if err := requireFields(object, map[string]bool{"profile_ref": true, "previous_profile_ref": true, "reason": true}); err != nil {
			return err
		}
		if refs, err := validateProfileRefs([]byte("[" + string(object["profile_ref"]) + "]")); err != nil || len(refs) != 1 {
			if err != nil {
				return err
			}
			return contractError(ProfileMalformed, "payload.profile_ref", "expected one profile reference")
		}
		if !bytes.Equal(bytes.TrimSpace(object["previous_profile_ref"]), []byte("null")) {
			if refs, err := validateProfileRefs([]byte("[" + string(object["previous_profile_ref"]) + "]")); err != nil || len(refs) != 1 {
				if err != nil {
					return err
				}
				return contractError(ProfileMalformed, "payload.previous_profile_ref", "expected one profile reference")
			}
		}
	case "BOOT":
		if err := requireFields(object, map[string]bool{"firmware_version": true, "reset_reason": true}); err != nil {
			return err
		}
		if _, err := decodeString(object, "firmware_version"); err != nil {
			return err
		}
		reason, err := decodeString(object, "reset_reason")
		if err != nil || !resetReasons[reason] {
			return contractError(InvalidEnum, "payload.reset_reason", "unknown reset reason")
		}
	case "HEALTH_SNAPSHOT":
		if err := requireFields(object, map[string]bool{"entries": true}); err != nil {
			return err
		}
		var entries []json.RawMessage
		if err := json.Unmarshal(object["entries"], &entries); err != nil || entries == nil {
			return contractError(MalformedJSON, "payload.entries", "expected array")
		}
		for index, entryRaw := range entries {
			entry, err := decodeObject(entryRaw, fmt.Sprintf("payload.entries[%d]", index))
			if err != nil {
				return err
			}
			if err := requireFields(entry, map[string]bool{"sensor_id": true, "health": true, "readiness": true, "last_attempt_monotonic_us": true, "last_ready_monotonic_us": true}); err != nil {
				return err
			}
			sensor, err := decodeString(entry, "sensor_id")
			if err != nil || !knownSensors[sensor] {
				return contractError(InvalidEnum, fmt.Sprintf("payload.entries[%d].sensor_id", index), "unknown health sensor")
			}
			health, err := decodeString(entry, "health")
			if err != nil || !healthStates[health] {
				return contractError(InvalidEnum, fmt.Sprintf("payload.entries[%d].health", index), "unknown health state")
			}
			readiness, err := decodeString(entry, "readiness")
			if err != nil || !knownQualities[readiness] {
				return contractError(InvalidEnum, fmt.Sprintf("payload.entries[%d].readiness", index), "unknown readiness")
			}
			if _, err := decodeUint64String(entry, "last_attempt_monotonic_us"); err != nil {
				return err
			}
			if !bytes.Equal(bytes.TrimSpace(entry["last_ready_monotonic_us"]), []byte("null")) {
				if _, err := decodeUint64String(entry, "last_ready_monotonic_us"); err != nil {
					return err
				}
			}
		}
	}
	if metadataType == "CONFIG_ACTIVATION" || metadataType == "PROFILE_ACTIVATION" {
		reason, err := decodeString(object, "reason")
		if err != nil || !activationReasons[reason] {
			return contractError(InvalidEnum, "payload.reason", "unknown activation reason")
		}
	}
	return nil
}

// ValidateBatch performs all pure v3 boundary checks without persistence or inference.
func ValidateBatch(body []byte) (*Batch, error) {
	// A body-level canonical parse enforces string limits before the per-record
	// boundary can be inspected. Preflight valid JSON envelopes so a single
	// oversized record is reported as RECORD_TOO_LARGE rather than as a nested
	// string error, while malformed/duplicate-key inputs still use strictJSON.
	if len(body) <= MaxBodyBytes {
		var envelope struct {
			Samples  []json.RawMessage `json:"samples"`
			Results  []json.RawMessage `json:"results"`
			Events   []json.RawMessage `json:"events"`
			Metadata []json.RawMessage `json:"metadata"`
		}
		if json.Unmarshal(body, &envelope) == nil {
			for _, records := range [][]json.RawMessage{envelope.Samples, envelope.Results, envelope.Events, envelope.Metadata} {
				for _, record := range records {
					if len(record) > MaxRecordBytes {
						return nil, contractError(RecordTooLarge, "", "record exceeds 16 KiB")
					}
				}
			}
		}
	}
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
