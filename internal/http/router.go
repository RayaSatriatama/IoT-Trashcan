// Package httpapi exposes the SIGAP HTTP API.
package httpapi

import (
	"context"
	"database/sql"
	"net/http"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

// IngestService is the narrow dependency required by the ingestion handler.
// It permits the HTTP contract to be tested independently from persistence.
type IngestService interface {
	Ingest(context.Context, string, ingest.Request, []byte) (ingest.Result, error)
}

type routerConfig struct {
	readAPI *ReadAPI
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

// NewRouter creates the HTTP router for the currently implemented API surface.
func NewRouter(service IngestService, options ...RouterOption) http.Handler {
	config := routerConfig{}
	for _, option := range options {
		if option != nil {
			option(&config)
		}
	}
	mux := http.NewServeMux()
	mux.Handle("POST /api/ingest", NewIngestHandler(service))
	if config.readAPI != nil {
		mux.Handle("GET /api/overview", http.HandlerFunc(config.readAPI.Overview))
		mux.Handle("GET /api/locations", http.HandlerFunc(config.readAPI.Locations))
		mux.Handle("GET /api/locations/{id}", http.HandlerFunc(config.readAPI.Location))
		mux.Handle("GET /api/devices/{id}", http.HandlerFunc(config.readAPI.Device))
		mux.Handle("GET /api/devices/{id}/research", http.HandlerFunc(config.readAPI.Research))
		mux.Handle("GET /api/history", http.HandlerFunc(config.readAPI.History))
		mux.Handle("GET /api/export.csv", http.HandlerFunc(config.readAPI.ExportCSV))
	}
	return mux
}
