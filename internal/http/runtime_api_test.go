package httpapi

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestRuntimeReportsSimulationAndIndicatorMetadata(t *testing.T) {
	router := NewRouter(nil, WithSimulationEnabled(true), WithSimulationIndicatorEnabled(false))
	request := httptest.NewRequest(http.MethodGet, "/api/runtime", nil)
	response := httptest.NewRecorder()
	router.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body map[string]bool
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if !body["simulation"] || body["simulationIndicator"] || len(body) != 2 {
		t.Fatalf("runtime metadata=%v", body)
	}
}

func TestRuntimeDefaultsToSimulationDisabled(t *testing.T) {
	router := NewRouter(nil)
	request := httptest.NewRequest(http.MethodGet, "/api/runtime", nil)
	response := httptest.NewRecorder()
	router.ServeHTTP(response, request)
	if response.Code != http.StatusOK || response.Body.String() != "{\"simulation\":false,\"simulationIndicator\":true}\n" {
		t.Fatalf("status=%d body=%q", response.Code, response.Body.String())
	}
}
