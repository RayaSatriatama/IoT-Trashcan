package handler

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestHandlerRoutesVercelRewriteToSnapshotAPI(t *testing.T) {
	t.Setenv("SIGAP_SIMULATION_ENABLED", "true")
	t.Setenv("SIGAP_SIMULATION_INTERVAL_SECONDS", "5")
	t.Setenv("SIGAP_SIMULATION_SEED", "42")
	t.Setenv("SIGAP_SIMULATION_INDICATOR_ENABLED", "false")
	t.Setenv("OFFLINE_AFTER_SECONDS", "15")

	overview := httptest.NewRecorder()
	Handler(overview, httptest.NewRequest(http.MethodGet, "/api?path=overview", nil))
	if overview.Code != http.StatusOK || !strings.Contains(overview.Body.String(), `"total_locations":8`) {
		t.Fatalf("overview status=%d body=%s", overview.Code, overview.Body.String())
	}

	runtime := httptest.NewRecorder()
	Handler(runtime, httptest.NewRequest(http.MethodGet, "/api?path=runtime", nil))
	if runtime.Code != http.StatusOK || !strings.Contains(runtime.Body.String(), `"simulation":true`) {
		t.Fatalf("runtime status=%d body=%s", runtime.Code, runtime.Body.String())
	}

	ingest := httptest.NewRecorder()
	Handler(ingest, httptest.NewRequest(http.MethodPost, "/api?path=ingest", nil))
	if ingest.Code != http.StatusMethodNotAllowed {
		t.Fatalf("ingest status=%d want=%d", ingest.Code, http.StatusMethodNotAllowed)
	}
}
