package httpapi

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

type queryHandlerAuthorizer struct {
	err error
}

func (a queryHandlerAuthorizer) AuthorizeDevice(context.Context, string, string) error {
	return a.err
}

type queryHandlerQuery struct{}

func (queryHandlerQuery) Current(context.Context, string) (telemetryarchive.CurrentSnapshot, error) {
	return telemetryarchive.CurrentSnapshot{}, nil
}

func TestTelemetryArchiveQueryRejectsNonGETWithMethodNotAllowed(t *testing.T) {
	recorder := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/api/telemetry/batches/device", nil)
	NewTelemetryArchiveQueryHandler(nil, queryHandlerAuthorizer{}).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status=%d, want %d", recorder.Code, http.StatusMethodNotAllowed)
	}
}

func TestTelemetryCurrentRejectsNonGETWithMethodNotAllowed(t *testing.T) {
	recorder := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/api/telemetry/current/device", nil)
	newTelemetryCurrentHandler(queryHandlerQuery{}, queryHandlerAuthorizer{}).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status=%d, want %d", recorder.Code, http.StatusMethodNotAllowed)
	}
}

func TestTelemetryQueryMisconfigurationIsServerError(t *testing.T) {
	tests := []struct {
		name    string
		handler http.Handler
	}{
		{
			name:    "archive query",
			handler: NewTelemetryArchiveQueryHandler(nil, nil),
		},
		{
			name:    "current query",
			handler: newTelemetryCurrentHandler(nil, nil),
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			recorder := httptest.NewRecorder()
			request := httptest.NewRequest(http.MethodGet, "/api/telemetry/batches/device", nil)
			test.handler.ServeHTTP(recorder, request)
			if recorder.Code != http.StatusInternalServerError {
				t.Fatalf("status=%d, want %d", recorder.Code, http.StatusInternalServerError)
			}
		})
	}
}
