package vercelsnapshot

import (
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestNewHandlerCreatesOneTickSimulationSnapshot(t *testing.T) {
	handler, err := NewHandler(Config{
		DatabasePath:               filepath.Join(t.TempDir(), "sigap-simulation.db"),
		SimulationEnabled:          true,
		SimulationIndicatorEnabled: false,
		SimulationSeed:             42,
		OfflineAfter:               15 * time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}

	overview := httptest.NewRecorder()
	handler.ServeHTTP(overview, httptest.NewRequest(http.MethodGet, "/api/overview", nil))
	if overview.Code != http.StatusOK {
		t.Fatalf("overview status=%d body=%s", overview.Code, overview.Body.String())
	}
	if got, want := overview.Body.String(), `"total_locations":8`; !strings.Contains(got, want) {
		t.Fatalf("overview=%s missing %s", got, want)
	}
	if got, want := overview.Body.String(), `"total_devices":8`; !strings.Contains(got, want) {
		t.Fatalf("overview=%s missing %s", got, want)
	}

	runtime := httptest.NewRecorder()
	handler.ServeHTTP(runtime, httptest.NewRequest(http.MethodGet, "/api/runtime", nil))
	if runtime.Code != http.StatusOK || !strings.Contains(runtime.Body.String(), `"simulation":true`) || !strings.Contains(runtime.Body.String(), `"simulationIndicator":false`) {
		t.Fatalf("runtime status=%d body=%s", runtime.Code, runtime.Body.String())
	}

	ingest := httptest.NewRecorder()
	handler.ServeHTTP(ingest, httptest.NewRequest(http.MethodPost, "/api/ingest", nil))
	if ingest.Code != http.StatusMethodNotAllowed {
		t.Fatalf("ingest status=%d want=%d", ingest.Code, http.StatusMethodNotAllowed)
	}
}
