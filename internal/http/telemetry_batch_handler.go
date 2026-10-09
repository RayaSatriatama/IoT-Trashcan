package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

// TelemetryBatchService is the narrow persistence boundary for telemetry-v3
// batches. The service authenticates the bearer token against the batch device,
// validates and durably stores the exact request bytes, and returns the ACK
// that is safe for the device journal to apply.
type TelemetryBatchService interface {
	IngestTelemetryBatch(context.Context, string, []byte) (telemetry.Ack, error)
}

var (
	// ErrTelemetryUnauthorized means the bearer credential cannot write for the
	// device identified by the batch.
	ErrTelemetryUnauthorized = errors.New("telemetry device unauthorized")
	// ErrTelemetryInvalid means the request failed a persistence-side contract
	// or semantic check.
	ErrTelemetryInvalid = errors.New("invalid telemetry batch")
	// ErrTelemetryConflict means a batch or record identity was reused with a
	// different immutable payload.
	ErrTelemetryConflict = errors.New("telemetry batch conflict")
	// ErrTelemetryPersistence means the durable archive was unavailable. The
	// device must retain the exact bytes and retry rather than treat this as an
	// ACK.
	ErrTelemetryPersistence = errors.New("telemetry archive unavailable")
)

// NewTelemetryBatchHandler creates POST /api/telemetry/batches. Router wiring
// is intentionally left to the composition root so the legacy ingest route is
// unchanged while the archive service can be injected independently.
func NewTelemetryBatchHandler(service TelemetryBatchService) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		if !isJSONContentType(r.Header.Get("Content-Type")) {
			writeError(w, http.StatusUnsupportedMediaType, "content type must be application/json")
			return
		}
		token, ok := bearerToken(r.Header.Get("Authorization"))
		if !ok {
			writeError(w, http.StatusUnauthorized, "unauthorized")
			return
		}

		r.Body = http.MaxBytesReader(w, r.Body, telemetry.MaxBodyBytes)
		raw, err := io.ReadAll(r.Body)
		if err != nil {
			writeError(w, http.StatusRequestEntityTooLarge, "telemetry batch exceeds 32 KiB")
			return
		}
		batch, err := telemetry.ValidateBatch(raw)
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid telemetry batch")
			return
		}
		if service == nil {
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		ack, err := service.IngestTelemetryBatch(r.Context(), token, raw)
		if err != nil {
			var contractErr *telemetry.ContractError
			switch {
			case errors.Is(err, ErrTelemetryUnauthorized), errors.Is(err, telemetryarchive.ErrUnauthorized):
				writeError(w, http.StatusUnauthorized, "unauthorized")
			case errors.Is(err, ErrTelemetryConflict), errors.Is(err, telemetryarchive.ErrConflict):
				writeError(w, http.StatusConflict, "conflicting telemetry replay")
			case errors.Is(err, ErrTelemetryInvalid), errors.As(err, &contractErr):
				writeError(w, http.StatusBadRequest, "invalid telemetry batch")
			case errors.Is(err, ErrTelemetryPersistence), errors.Is(err, telemetryarchive.ErrPersistence):
				writeError(w, http.StatusServiceUnavailable, "telemetry archive unavailable")
			default:
				writeError(w, http.StatusInternalServerError, "internal server error")
			}
			return
		}

		// Validate the service result before publishing it as a durable ACK. This
		// prevents a buggy adapter from advancing the device's journal with an
		// identity, hash, or count unrelated to the exact request bytes.
		ackRaw, err := marshalAck(ack)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		if _, err := telemetry.ValidateAck(ackRaw, telemetry.AckExpectation{
			DeviceID:    batch.DeviceID,
			BatchID:     batch.BatchID,
			BodyHash:    telemetry.BodyHash(raw),
			RecordCount: uint32(batch.RecordCount),
		}); err != nil {
			writeError(w, http.StatusInternalServerError, "invalid archive acknowledgement")
			return
		}
		writeJSON(w, http.StatusOK, ack)
	})
}

func marshalAck(ack telemetry.Ack) ([]byte, error) {
	return json.Marshal(ack)
}
