package handler

import (
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/simulator"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/vercelsnapshot"
)

var (
	initialize sync.Once
	handler    http.Handler
	initErr    error
)

// Handler is the Vercel Go Function entrypoint for every rewritten /api/* request.
func Handler(w http.ResponseWriter, r *http.Request) {
	initialize.Do(func() {
		handler, initErr = newSnapshotHandler()
	})
	if initErr != nil {
		http.Error(w, "simulation snapshot unavailable", http.StatusServiceUnavailable)
		return
	}
	path := strings.TrimPrefix(r.URL.Query().Get("path"), "/")
	if path != "" {
		r = r.Clone(r.Context())
		r.URL.Path = "/api/" + path
		r.URL.RawQuery = r.URL.Query().Encode()
	}
	handler.ServeHTTP(w, r)
}

func newSnapshotHandler() (http.Handler, error) {
	simulation, err := simulator.ParseEnvironment()
	if err != nil {
		return nil, err
	}
	indicator, err := boolEnv("SIGAP_SIMULATION_INDICATOR_ENABLED", true)
	if err != nil {
		return nil, err
	}
	offlineAfter, err := secondsEnv("OFFLINE_AFTER_SECONDS")
	if err != nil {
		return nil, err
	}
	return vercelsnapshot.NewHandler(vercelsnapshot.Config{
		DatabasePath:               filepath.Join(os.TempDir(), "sigap-simulation.db"),
		SimulationEnabled:          simulation.Enabled,
		SimulationIndicatorEnabled: indicator,
		SimulationSeed:             simulation.Seed,
		OfflineAfter:               offlineAfter,
	})
}

func boolEnv(name string, fallback bool) (bool, error) {
	value := os.Getenv(name)
	if value == "" {
		return fallback, nil
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		return false, fmt.Errorf("%s must be true or false", name)
	}
	return parsed, nil
}

func secondsEnv(name string) (time.Duration, error) {
	value := os.Getenv(name)
	if value == "" {
		return 0, nil
	}
	seconds, err := strconv.ParseInt(value, 10, 64)
	if err != nil || seconds <= 0 {
		return 0, fmt.Errorf("%s must be a positive integer", name)
	}
	return time.Duration(seconds) * time.Second, nil
}
