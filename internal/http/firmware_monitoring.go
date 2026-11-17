package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"sort"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

// Current monitoring scalars only. Protected calibration profiles, ROMs,
// history, authorization and command results are deliberately not projected.
type firmwareMonitoringDTO struct {
	SensorID        string             `json:"sensor_id"`
	Stream          string             `json:"stream"`
	BootID          string             `json:"boot_id"`
	Quality         string             `json:"quality"`
	Reason          string             `json:"reason"`
	SampleOrigin    string             `json:"sample_origin"`
	FirmwareVersion *string            `json:"origin_firmware_version"`
	Sequence        string             `json:"last_sequence"`
	Raw             map[string]float64 `json:"raw"`
	Converted       map[string]float64 `json:"converted_value"`
	Fresh           *bool              `json:"fresh"`
	AgeMS           *int64             `json:"age_ms"`
	OrderingUnknown bool               `json:"ordering_unknown"`
	FutureClockSkew bool               `json:"future_clock_skew"`
}

var monitoringRawKeys = []string{"counts", "adc_counts", "echo_us", "temperature_ticks", "humidity_ticks", "temperature_raw"}
var monitoringConvertedKeys = []string{"kg", "distance_mm", "fill_percent", "adc_mv", "temperature_c", "humidity_percent"}

func monitoringConvertedKeysFor(sensorID string) []string {
	if sensorID != "mq135" {
		return monitoringConvertedKeys
	}
	return append(append([]string(nil), monitoringConvertedKeys...), "adc_pin_mv", "sensor_ao_mv", "adc_efuse_calibrated")
}

func monitoringNumbers(raw json.RawMessage, keys []string) map[string]float64 {
	var fields map[string]json.RawMessage
	if json.Unmarshal(raw, &fields) != nil {
		return nil
	}
	var result map[string]float64
	for _, key := range keys {
		var value *float64
		if json.Unmarshal(fields[key], &value) != nil || value == nil || math.IsNaN(*value) || math.IsInf(*value, 0) {
			continue
		}
		if result == nil {
			result = make(map[string]float64)
		}
		result[key] = *value
	}
	return result
}

func (api *ReadAPI) firmwareMonitoring(ctx context.Context, owner string) ([]firmwareMonitoringDTO, error) {
	result := make([]firmwareMonitoringDTO, 0)
	snapshot, err := telemetryarchive.Current(ctx, api.db, owner)
	if err != nil {
		return nil, err
	}
	// Discovery contributes only bounded sensor identities, never metadata from
	// another read. LatestDiagnostic returns each complete immutable sample and
	// its consistent provenance/time interpretation even during concurrent ingest.
	identities := make(map[string]struct{})
	for _, sensors := range snapshot.Samples {
		for sensor := range sensors {
			identities[sensor] = struct{}{}
		}
	}
	names := make([]string, 0, len(identities))
	for sensor := range identities {
		names = append(names, sensor)
	}
	sort.Strings(names)
	store := devicecontrol.NewStore(api.db).WithClock(api.now)
	for _, sensor := range names {
		encoded, err := store.LatestDiagnostic(ctx, owner, sensor)
		if errors.Is(err, devicecontrol.ErrNotFound) {
			continue
		}
		if err != nil {
			return nil, err
		}
		var item struct {
			firmwareMonitoringDTO
			Raw       json.RawMessage `json:"raw"`
			Converted json.RawMessage `json:"converted_value"`
		}
		if err := json.Unmarshal(encoded, &item); err != nil {
			return nil, err
		}
		view := item.firmwareMonitoringDTO
		view.Raw = monitoringNumbers(item.Raw, monitoringRawKeys)
		// Uncalibrated raw values are useful; they must not be disguised as
		// calibrated engineering values or feed legacy operational KPIs.
		if view.Quality == "READY" {
			view.Converted = monitoringNumbers(item.Converted, monitoringConvertedKeysFor(sensor))
		}
		result = append(result, view)
	}
	return result, nil
}
