package httpapi

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"mime"
	"net/http"
	"strings"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

const maxIngestBodyBytes = 1 << 20

// NewIngestHandler returns the authenticated raw-reading ingestion endpoint.
func NewIngestHandler(service IngestService) http.Handler {
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

		r.Body = http.MaxBytesReader(w, r.Body, maxIngestBodyBytes)
		rawPayload, err := io.ReadAll(r.Body)
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid request body")
			return
		}

		var request ingest.Request
		if err := decodeJSONObject(rawPayload, &request); err != nil {
			writeError(w, http.StatusBadRequest, "invalid JSON payload")
			return
		}

		result, err := service.Ingest(r.Context(), token, request, rawPayload)
		if err != nil {
			switch {
			case errors.Is(err, ingest.ErrUnauthorized):
				writeError(w, http.StatusUnauthorized, "unauthorized")
			case errors.Is(err, ingest.ErrNotFound):
				writeError(w, http.StatusNotFound, "not found")
			case errors.Is(err, ingest.ErrInvalidRequest):
				writeError(w, http.StatusBadRequest, "invalid ingestion payload")
			case errors.Is(err, ingest.ErrConflict):
				writeError(w, http.StatusConflict, "conflicting ingestion replay")
			default:
				writeError(w, http.StatusInternalServerError, "internal server error")
			}
			return
		}

		writeJSON(w, http.StatusOK, result)
	})
}

func decodeJSONObject(raw []byte, destination *ingest.Request) error {
	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) == 0 || trimmed[0] != '{' {
		return errors.New("payload must be a JSON object")
	}

	decoder := json.NewDecoder(bytes.NewReader(trimmed))
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("trailing JSON value")
		}
		return err
	}
	return nil
}

func isJSONContentType(value string) bool {
	mediaType, _, err := mime.ParseMediaType(value)
	return err == nil && strings.EqualFold(mediaType, "application/json")
}

func bearerToken(value string) (string, bool) {
	parts := strings.Fields(value)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "bearer") || parts[1] == "" {
		return "", false
	}
	return parts[1], true
}

func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]string{"error": message})
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(payload)
}
