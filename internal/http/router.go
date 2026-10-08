// Package httpapi exposes the SIGAP HTTP API.
package httpapi

import (
	"context"
	"database/sql"
	"net/http"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

// IngestService is the narrow dependency required by the ingestion handler.
// It permits the HTTP contract to be tested independently from persistence.
type IngestService interface {
	Ingest(context.Context, string, ingest.Request, []byte) (ingest.Result, error)
}

type routerConfig struct {
	readAPI             *ReadAPI
	telemetryBatch      http.Handler
	telemetryArchive    http.Handler
	telemetryCurrent    http.Handler
	telemetryRecords    http.Handler
	simulation          bool
	simulationIndicator bool
}

type runtimeMetadata struct {
	Simulation          bool `json:"simulation"`
	SimulationIndicator bool `json:"simulationIndicator"`
}

// RouterOption composes optional API surfaces without changing the stable
// ingestion handler dependency used by existing callers and tests.
type RouterOption func(*routerConfig)

// WithReadAPI installs the monitoring read endpoints.
func WithReadAPI(api *ReadAPI) RouterOption {
	return func(config *routerConfig) { config.readAPI = api }
}

// WithReadDatabase creates the monitoring read service over the supplied
// database. ReadAPIOption values keep clocks and offline policy injectable.
func WithReadDatabase(db *sql.DB, options ...ReadAPIOption) RouterOption {
	return WithReadAPI(NewReadAPI(db, options...))
}

// WithSimulationEnabled exposes only the safe runtime mode flag. It never
// exposes simulator credentials or internal configuration.
func WithSimulationEnabled(enabled bool) RouterOption {
	return func(config *routerConfig) { config.simulation = enabled }
}

// WithSimulationIndicatorEnabled controls whether the web client displays the
// simulation indicator. The runtime simulation state remains available.
func WithSimulationIndicatorEnabled(enabled bool) RouterOption {
	return func(config *routerConfig) { config.simulationIndicator = enabled }
}

// WithTelemetryArchive installs the additive telemetry-v3 write and read
// surfaces. The legacy summary ingest route is not changed.
func WithTelemetryArchive(service *telemetryarchive.Service, query *telemetryarchive.ArchiveQuery) RouterOption {
	return func(config *routerConfig) {
		config.telemetryBatch = NewTelemetryBatchHandler(service)
		config.telemetryArchive = NewTelemetryArchiveQueryHandler(query, service)
		config.telemetryCurrent = newTelemetryCurrentHandler(query, service)
		config.telemetryRecords = newTelemetryRecordArchiveHandler(query, service)
	}
}

// NewRouter creates the HTTP router for the currently implemented API surface.
func NewRouter(service IngestService, options ...RouterOption) http.Handler {
	config := routerConfig{simulationIndicator: true}
	for _, option := range options {
		if option != nil {
			option(&config)
		}
	}
	mux := http.NewServeMux()
	mux.Handle("POST /api/ingest", NewIngestHandler(service))
	if config.telemetryBatch != nil {
		mux.Handle("POST /api/telemetry/batches", config.telemetryBatch)
	}
	if config.telemetryArchive != nil {
		mux.Handle("GET /api/telemetry/batches/{device_id}", config.telemetryArchive)
		mux.Handle("GET /api/telemetry/batches/{device_id}/{batch_id...}", config.telemetryArchive)
		mux.Handle("GET /api/telemetry/current/{device_id}", config.telemetryCurrent)
		mux.Handle("GET /api/telemetry/records/{device_id}", config.telemetryRecords)
		mux.Handle("GET /api/telemetry/records/{device_id}/export", config.telemetryRecords)
	}
	if config.readAPI != nil {
		mux.Handle("GET /api/overview", http.HandlerFunc(config.readAPI.Overview))
		mux.Handle("GET /api/locations", http.HandlerFunc(config.readAPI.Locations))
		mux.Handle("GET /api/locations/{id}", http.HandlerFunc(config.readAPI.Location))
		mux.Handle("GET /api/devices", http.HandlerFunc(config.readAPI.Devices))
		mux.Handle("GET /api/devices/{id}", http.HandlerFunc(config.readAPI.Device))
		mux.Handle("GET /api/devices/{id}/research", http.HandlerFunc(config.readAPI.Research))
		mux.Handle("GET /api/history", http.HandlerFunc(config.readAPI.History))
		mux.Handle("GET /api/export.csv", http.HandlerFunc(config.readAPI.ExportCSV))
	}
	mux.Handle("GET /api/runtime", http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, runtimeMetadata{
			Simulation:          config.simulation,
			SimulationIndicator: config.simulationIndicator,
		})
	}))
	return mux
}
