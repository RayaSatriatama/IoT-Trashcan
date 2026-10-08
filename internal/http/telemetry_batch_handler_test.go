package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
)

const emptyTelemetryBatch = `{"schema_version":3,"device_id":"device-1","batch_id":"device-1/boot-1/batch-1","record_count":0,"samples":[],"results":[],"events":[],"metadata":[]}`

type fakeTelemetryBatchService struct {
	called bool
	token  string
	body   []byte
	ack    telemetry.Ack
	err    error
}

func (f *fakeTelemetryBatchService) IngestTelemetryBatch(_ context.Context, token string, body []byte) (telemetry.Ack, error) {
	f.called = true
	f.token = token
	f.body = append([]byte(nil), body...)
	if f.err != nil {
		return telemetry.Ack{}, f.err
	}
	return f.ack, nil
}

func validBatchAck(body []byte) telemetry.Ack {
	return telemetry.Ack{
		DeviceID:          "device-1",
		BatchID:           "device-1/boot-1/batch-1",
		BodyHash:          telemetry.BodyHash(body),
		RecordCount:       0,
		StoredCount:       0,
		DeduplicatedCount: 0,
		Status:            "DURABLE",
	}
}

func batchRequest(body string) *http.Request {
	request := httptest.NewRequest(http.MethodPost, "/api/telemetry/batches", strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer fixture-token")
	return request
}

func TestTelemetryBatchHandlerAcceptsValidatedDurableAck(t *testing.T) {
	service := &fakeTelemetryBatchService{}
	service.ack = validBatchAck([]byte(emptyTelemetryBatch))
	recorder := httptest.NewRecorder()

	NewTelemetryBatchHandler(service).ServeHTTP(recorder, batchRequest(emptyTelemetryBatch))

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: %s", recorder.Code, recorder.Body.String())
	}
	if !service.called || service.token != "fixture-token" || string(service.body) != emptyTelemetryBatch {
		t.Fatalf("service call = %v token=%q body=%q", service.called, service.token, service.body)
	}
	var got telemetry.Ack
	if err := json.Unmarshal(recorder.Body.Bytes(), &got); err != nil {
		t.Fatalf("decode ACK: %v", err)
	}
	if got != service.ack {
		t.Fatalf("ACK = %#v, want %#v", got, service.ack)
	}
}

func TestTelemetryBatchHandlerRejectsMissingBearerBeforeService(t *testing.T) {
	service := &fakeTelemetryBatchService{}
	request := batchRequest(emptyTelemetryBatch)
	request.Header.Del("Authorization")
	recorder := httptest.NewRecorder()

	NewTelemetryBatchHandler(service).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusUnauthorized || service.called {
		t.Fatalf("status=%d called=%v, want 401 and false", recorder.Code, service.called)
	}
}

func TestTelemetryBatchHandlerRejectsMalformedAndOversizedBodies(t *testing.T) {
	tests := []struct {
		name string
		body string
		want int
	}{
		{name: "malformed", body: `{"schema_version":`, want: http.StatusBadRequest},
		{name: "oversized", body: strings.Repeat("x", telemetry.MaxBodyBytes+1), want: http.StatusRequestEntityTooLarge},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			service := &fakeTelemetryBatchService{}
			recorder := httptest.NewRecorder()
			NewTelemetryBatchHandler(service).ServeHTTP(recorder, batchRequest(test.body))
			if recorder.Code != test.want || service.called {
				t.Fatalf("status=%d called=%v, want %d and false", recorder.Code, service.called, test.want)
			}
		})
	}
}

func TestTelemetryBatchHandlerRejectsUnsupportedContentType(t *testing.T) {
	service := &fakeTelemetryBatchService{}
	request := batchRequest(emptyTelemetryBatch)
	request.Header.Set("Content-Type", "text/plain")
	recorder := httptest.NewRecorder()

	NewTelemetryBatchHandler(service).ServeHTTP(recorder, request)

	if recorder.Code != http.StatusUnsupportedMediaType || service.called {
		t.Fatalf("status=%d called=%v, want 415 and false", recorder.Code, service.called)
	}
}

func TestTelemetryBatchHandlerMapsArchiveErrors(t *testing.T) {
	tests := []struct {
		name string
		err  error
		want int
	}{
		{name: "unauthorized", err: ErrTelemetryUnauthorized, want: http.StatusUnauthorized},
		{name: "invalid", err: ErrTelemetryInvalid, want: http.StatusBadRequest},
		{name: "conflict", err: ErrTelemetryConflict, want: http.StatusConflict},
		{name: "persistence", err: ErrTelemetryPersistence, want: http.StatusServiceUnavailable},
		{name: "unknown", err: errors.New("unexpected"), want: http.StatusInternalServerError},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			service := &fakeTelemetryBatchService{err: test.err}
			recorder := httptest.NewRecorder()
			NewTelemetryBatchHandler(service).ServeHTTP(recorder, batchRequest(emptyTelemetryBatch))
			if recorder.Code != test.want {
				t.Fatalf("status=%d, want %d", recorder.Code, test.want)
			}
		})
	}
}

func TestTelemetryBatchHandlerRejectsInvalidServiceAck(t *testing.T) {
	service := &fakeTelemetryBatchService{ack: telemetry.Ack{Status: "DURABLE"}}
	recorder := httptest.NewRecorder()

	NewTelemetryBatchHandler(service).ServeHTTP(recorder, batchRequest(emptyTelemetryBatch))

	if recorder.Code != http.StatusInternalServerError {
		t.Fatalf("status=%d, want 500: %s", recorder.Code, recorder.Body.String())
	}
}
