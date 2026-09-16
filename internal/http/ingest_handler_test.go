package httpapi

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

const validPayload = `{"device_id":"device-1","recorded_at":"2026-09-16T10:00:00Z","health":{"battery":90},"slots":[{"stream":"organic","fill":1},{"stream":"inorganic","fill":2},{"stream":"b3","fill":3}]}`

type fakeIngestService struct {
	called bool
	token  string
	raw    []byte
	err    error
}

func (f *fakeIngestService) Ingest(_ context.Context, token string, _ ingest.Request, raw []byte) (ingest.Result, error) {
	f.called = true
	f.token = token
	f.raw = raw
	if f.err != nil {
		return ingest.Result{}, f.err
	}
	return ingest.Result{DeviceID: "device-1", RecordedAt: time.Date(2026, 9, 16, 10, 0, 0, 0, time.UTC), ReadingsStored: 3}, nil
}

func TestIngestHandlerAcceptsAuthenticatedJSONObject(t *testing.T) {
	service := &fakeIngestService{}
	request := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(validPayload))
	request.Header.Set("Content-Type", "application/json; charset=utf-8")
	request.Header.Set("Authorization", "Bearer fixture-token")
	recorder := httptest.NewRecorder()

	NewRouter(service).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d: %s", recorder.Code, http.StatusOK, recorder.Body.String())
	}
	if !service.called || service.token != "fixture-token" || string(service.raw) != validPayload {
		t.Fatalf("service received called=%v token=%q raw=%q", service.called, service.token, service.raw)
	}
	if !strings.Contains(recorder.Body.String(), `"readings_stored":3`) {
		t.Fatalf("response does not contain result: %s", recorder.Body.String())
	}
}

func TestIngestHandlerRejectsMissingBearerBeforeService(t *testing.T) {
	service := &fakeIngestService{}
	request := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(validPayload))
	request.Header.Set("Content-Type", "application/json")
	recorder := httptest.NewRecorder()

	NewRouter(service).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusUnauthorized || service.called {
		t.Fatalf("status=%d called=%v, want 401 and false", recorder.Code, service.called)
	}
}

func TestIngestHandlerRejectsUnsupportedContentType(t *testing.T) {
	service := &fakeIngestService{}
	request := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(validPayload))
	request.Header.Set("Content-Type", "text/plain")
	request.Header.Set("Authorization", "Bearer fixture-token")
	recorder := httptest.NewRecorder()

	NewRouter(service).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("status=%d, want %d", recorder.Code, http.StatusUnsupportedMediaType)
	}
	if service.called {
		t.Fatal("service was called for unsupported content type")
	}
}

func TestIngestHandlerRejectsOversizedBody(t *testing.T) {
	service := &fakeIngestService{}
	payload := `{"padding":"` + strings.Repeat("x", maxIngestBodyBytes) + `"}`
	if len(payload) <= maxIngestBodyBytes {
		t.Fatal("test payload does not exceed body limit")
	}
	request := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer fixture-token")
	recorder := httptest.NewRecorder()

	NewRouter(service).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status=%d, want %d", recorder.Code, http.StatusBadRequest)
	}
	if service.called {
		t.Fatal("service was called for oversized body")
	}
}

func TestIngestHandlerRejectsMalformedOrTrailingJSON(t *testing.T) {
	for _, payload := range []string{`{"device_id":`, validPayload + ` {}`} {
		t.Run(payload[:1], func(t *testing.T) {
			service := &fakeIngestService{}
			request := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(payload))
			request.Header.Set("Content-Type", "application/json")
			request.Header.Set("Authorization", "Bearer fixture-token")
			recorder := httptest.NewRecorder()

			NewRouter(service).ServeHTTP(recorder, request)

			if recorder.Code != http.StatusBadRequest || service.called {
				t.Fatalf("status=%d called=%v, want 400 and false", recorder.Code, service.called)
			}
		})
	}
}

func TestIngestHandlerMapsServiceErrors(t *testing.T) {
	tests := []struct {
		name string
		err  error
		want int
	}{
		{"unauthorized", ingest.ErrUnauthorized, http.StatusUnauthorized},
		{"invalid", ingest.ErrInvalidRequest, http.StatusBadRequest},
		{"persistence", errors.New("database unavailable"), http.StatusInternalServerError},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			service := &fakeIngestService{err: test.err}
			request := httptest.NewRequest(http.MethodPost, "/api/ingest", strings.NewReader(validPayload))
			request.Header.Set("Content-Type", "application/json")
			request.Header.Set("Authorization", "Bearer fixture-token")
			recorder := httptest.NewRecorder()

			NewRouter(service).ServeHTTP(recorder, request)

			if recorder.Code != test.want {
				t.Fatalf("status=%d, want %d: %s", recorder.Code, test.want, recorder.Body.String())
			}
		})
	}
}
