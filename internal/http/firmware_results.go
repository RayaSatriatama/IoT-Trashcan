package httpapi

import (
	"context"
	"encoding/json"
	"sort"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

type firmwareTraceDTO struct {
	ModelRef struct {
		Version string `json:"version"`
		Hash    string `json:"hash"`
	} `json:"model_ref"`
	Memberships []struct {
		Input string  `json:"input"`
		Set   string  `json:"set"`
		Value float64 `json:"value"`
	} `json:"memberships"`
	Rules []struct {
		ID       string  `json:"id"`
		OutputID string  `json:"output_id"`
		Strength float64 `json:"strength"`
	} `json:"rules"`
	Aggregate *struct {
		OutputID string   `json:"output_id"`
		Area     *float64 `json:"area"`
		Centroid *float64 `json:"centroid"`
	} `json:"aggregate"`
}

type firmwareProcessedDTO struct {
	FillPct              *float64 `json:"fill_pct"`
	WeightKG             *float64 `json:"weight_kg"`
	WeightUtilizationPct *float64 `json:"weight_utilization_pct"`
	OGI                  *float64 `json:"ogi"`
	AccumulationHours    *float64 `json:"accumulation_hours"`
	TemperatureC         *float64 `json:"temperature_c"`
}

type firmwareFuzzyDTO struct {
	State      string            `json:"state"`
	Score      *float64          `json:"score"`
	Label      *string           `json:"label"`
	Reason     string            `json:"reason"`
	ModelState string            `json:"model_state"`
	Trace      *firmwareTraceDTO `json:"trace"`
}

type firmwareSafetyDTO struct {
	State      string            `json:"state"`
	Reason     string            `json:"reason"`
	ModelState string            `json:"model_state"`
	Trace      *firmwareTraceDTO `json:"trace"`
}

// firmwareResultDTO intentionally exposes only the public MCU result contract;
// archive identity hashes, configuration references, and profiles stay private.
type firmwareResultDTO struct {
	Stream                string               `json:"stream"`
	BootID                string               `json:"boot_id"`
	Sequence              string               `json:"sequence"`
	MonotonicUS           string               `json:"monotonic_us"`
	RecordedAt            *string              `json:"recorded_at"`
	TimeQuality           string               `json:"time_quality"`
	SourceSampleIDs       []string             `json:"source_sample_ids"`
	Processed             firmwareProcessedDTO `json:"processed"`
	Fuzzy                 firmwareFuzzyDTO     `json:"fuzzy"`
	Safety                firmwareSafetyDTO    `json:"safety"`
	OrderingUnknown       bool                 `json:"ordering_unknown"`
	ResultOrigin          string               `json:"result_origin"`
	OriginFirmwareVersion *string              `json:"origin_firmware_version"`
	Fresh                 *bool                `json:"fresh"`
	AgeMS                 *int64               `json:"age_ms"`
	FutureClockSkew       bool                 `json:"future_clock_skew"`
}

func (api *ReadAPI) firmwareResults(ctx context.Context, owner string) ([]firmwareResultDTO, error) {
	snapshot, err := telemetryarchive.Current(ctx, api.db, owner)
	if err != nil {
		return nil, err
	}
	streams := make([]string, 0, len(snapshot.Results))
	for stream := range snapshot.Results {
		streams = append(streams, stream)
	}
	order := map[string]int{"organic": 0, "inorganic": 1, "b3": 2}
	sort.Slice(streams, func(i, j int) bool { return order[streams[i]] < order[streams[j]] })
	unknown := make(map[string]bool, len(snapshot.OrderingUnknown))
	for _, key := range snapshot.OrderingUnknown {
		unknown[key] = true
	}
	results := make([]firmwareResultDTO, 0, 3)
	for _, stream := range streams {
		if _, ok := order[stream]; !ok {
			continue
		}
		var item firmwareResultDTO
		if err := json.Unmarshal(snapshot.Results[stream], &item); err != nil {
			return nil, err
		}
		if item.Stream != stream {
			continue
		}
		origin, version := devicecontrol.DiagnosticOriginForBoot(ctx, api.db, owner, item.BootID)
		item.ResultOrigin = string(origin)
		if version != "" {
			item.OriginFirmwareVersion = &version
		}
		item.OrderingUnknown = unknown[stream]
		if item.TimeQuality != "UNSYNCED" && item.RecordedAt != nil {
			if stamp, err := time.Parse(time.RFC3339Nano, *item.RecordedAt); err == nil {
				now := api.now()
				if stamp.After(now) {
					item.FutureClockSkew = true
				} else {
					age := now.Sub(stamp).Milliseconds()
					item.AgeMS = &age
				}
			}
		}
		results = append(results, item)
	}
	return results, nil
}
