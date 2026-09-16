// Package httpapi exposes the SIGAP HTTP API.
package httpapi

import (
	"context"
	"net/http"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

// IngestService is the narrow dependency required by the ingestion handler.
// It permits the HTTP contract to be tested independently from persistence.
type IngestService interface {
	Ingest(context.Context, string, ingest.Request, []byte) (ingest.Result, error)
}

// NewRouter creates the HTTP router for the currently implemented API surface.
func NewRouter(service IngestService) http.Handler {
	mux := http.NewServeMux()
	mux.Handle("POST /api/ingest", NewIngestHandler(service))
	return mux
}
