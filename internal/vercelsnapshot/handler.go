// Package vercelsnapshot creates a small, read-only simulation dataset for a
// single Vercel Function instance. It deliberately does not start the local
// simulator loop because Vercel Functions are not durable processes.
package vercelsnapshot

import (
	"context"
	"fmt"
	"net/http"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	httpapi "github.com/RayaSatriatama/IoT-Trashcan/internal/http"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/simulator"
)

// Config defines the Vercel-local simulation snapshot.
type Config struct {
	DatabasePath               string
	SimulationEnabled          bool
	SimulationIndicatorEnabled bool
	SimulationSeed             int64
	OfflineAfter               time.Duration
}

// NewHandler seeds the compact simulation dataset and, when enabled, writes
// one deterministic telemetry tick. Mutation endpoints are intentionally not
// exposed because a Vercel Function's temporary filesystem is not durable.
func NewHandler(config Config) (http.Handler, error) {
	if config.DatabasePath == "" {
		return nil, fmt.Errorf("snapshot database path is required")
	}
	db, err := database.Open(config.DatabasePath)
	if err != nil {
		return nil, err
	}

	serviceOptions := make([]ingest.ServiceOption, 0, 2)
	if config.SimulationEnabled {
		serviceOptions = append(serviceOptions,
			ingest.WithDecisionProfiles(simulator.DemoEngines()),
			ingest.WithSafetyConfig(simulator.DemoSafetyConfig()),
		)
	}
	service := ingest.NewService(db, serviceOptions...)
	if config.SimulationEnabled {
		dataset, seedErr := simulator.Seed(context.Background(), db, config.SimulationSeed)
		if seedErr != nil {
			db.Close()
			return nil, seedErr
		}
		snapshot := simulator.New(service, dataset, simulator.Config{
			Enabled:  true,
			Interval: time.Second,
			Seed:     config.SimulationSeed,
		})
		if tickErr := snapshot.Tick(context.Background()); tickErr != nil {
			db.Close()
			return nil, tickErr
		}
	}

	readOptions := make([]httpapi.ReadAPIOption, 0, 1)
	if config.OfflineAfter > 0 {
		readOptions = append(readOptions, httpapi.WithOfflineAfter(config.OfflineAfter))
	}
	api := httpapi.NewRouter(
		service,
		httpapi.WithReadDatabase(db, readOptions...),
		httpapi.WithSimulationEnabled(config.SimulationEnabled),
		httpapi.WithSimulationIndicatorEnabled(config.SimulationIndicatorEnabled),
	)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/ingest" {
			w.Header().Set("Allow", http.MethodGet)
			http.Error(w, "Vercel simulation snapshot is read-only", http.StatusMethodNotAllowed)
			return
		}
		api.ServeHTTP(w, r)
	}), nil
}
