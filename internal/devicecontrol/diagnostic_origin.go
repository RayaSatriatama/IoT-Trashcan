package devicecontrol

import (
	"context"
	"database/sql"
	"encoding/json"
	"strings"
)

type SampleOrigin string

const (
	OriginSimulated SampleOrigin = "SIMULATED"
	OriginPhysical  SampleOrigin = "PHYSICAL"
	OriginUnknown   SampleOrigin = "UNKNOWN"
)

func classifyDiagnosticOrigin(tags []string) (SampleOrigin, string) {
	if len(tags) != 1 || strings.TrimSpace(tags[0]) != tags[0] {
		return OriginUnknown, ""
	}
	switch tags[0] {
	case "sigap-task6-sensor-sim-v1":
		return OriginSimulated, tags[0]
	case "sigap-task6", "sigap-task6-physical4-v1", "sigap-task6-physical4-v2":
		return OriginPhysical, tags[0]
	default:
		return OriginUnknown, ""
	}
}

// DiagnosticOriginForBoot exposes the existing fail-closed classifier to read projections.
func DiagnosticOriginForBoot(ctx context.Context, db *sql.DB, deviceID, bootID string) (SampleOrigin, string) {
	return diagnosticOrigin(ctx, db, deviceID, bootID)
}

func diagnosticOrigin(ctx context.Context, db *sql.DB, deviceID, bootID string) (SampleOrigin, string) {
	if db == nil || deviceID == "" || bootID == "" {
		return OriginUnknown, ""
	}
	// Filter before bounding: unrelated metadata must neither hide BOOT nor
	// conceal a second, conflicting declaration. Two BOOT rows suffice to fail closed.
	rows, err := db.QueryContext(ctx, `SELECT record_bytes FROM telemetry_records
		WHERE device_id=? AND boot_id=? AND kind='metadata'
		AND json_extract(CAST(record_bytes AS TEXT), '$.metadata_type')='BOOT' LIMIT 2`, deviceID, bootID)
	if err != nil {
		return OriginUnknown, ""
	}
	defer rows.Close()
	var tags []string
	for rows.Next() {
		var raw []byte
		if rows.Scan(&raw) != nil {
			return OriginUnknown, ""
		}
		var record struct {
			MetadataType string `json:"metadata_type"`
			Payload      struct {
				FirmwareVersion json.RawMessage `json:"firmware_version"`
			} `json:"payload"`
		}
		if json.Unmarshal(raw, &record) != nil || record.MetadataType != "BOOT" {
			continue
		}
		var version string
		if len(record.Payload.FirmwareVersion) == 0 || json.Unmarshal(record.Payload.FirmwareVersion, &version) != nil {
			return OriginUnknown, ""
		}
		tags = append(tags, version)
	}
	if rows.Err() != nil || len(tags) != 1 {
		return OriginUnknown, ""
	}
	return classifyDiagnosticOrigin(tags)
}
