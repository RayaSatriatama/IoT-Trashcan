package httpapi

import (
	"context"
	"errors"
	"net/http"
	"strconv"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

type telemetryArchiveAuthorizer interface {
	AuthorizeDevice(context.Context, string, string) error
}

type telemetryArchiveQuery interface {
	ListBatches(context.Context, string, int, int) ([]telemetryarchive.BatchManifest, error)
	LoadBatch(context.Context, string, string) (telemetryarchive.StoredBatch, error)
}

// NewTelemetryArchiveQueryHandler exposes bounded, authenticated manifest and
// exact-body reads. It never rebuilds a batch through a current serializer.
func NewTelemetryArchiveQueryHandler(query telemetryArchiveQuery, authorizer telemetryArchiveAuthorizer) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		deviceID := r.PathValue("device_id")
		token, ok := bearerToken(r.Header.Get("Authorization"))
		if query == nil || authorizer == nil {
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		if !ok {
			writeError(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		if err := authorizer.AuthorizeDevice(r.Context(), deviceID, token); err != nil {
			if errors.Is(err, telemetryarchive.ErrUnauthorized) {
				writeError(w, http.StatusUnauthorized, "unauthorized")
			} else {
				writeError(w, http.StatusInternalServerError, "internal server error")
			}
			return
		}

		batchID := r.PathValue("batch_id")
		if batchID != "" {
			batch, err := query.LoadBatch(r.Context(), deviceID, batchID)
			if errors.Is(err, telemetryarchive.ErrNotFound) {
				writeError(w, http.StatusNotFound, "not found")
				return
			}
			if err != nil {
				writeError(w, http.StatusInternalServerError, "internal server error")
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(batch.Body)
			return
		}

		limit, offset, err := archivePage(r)
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid archive page")
			return
		}
		manifests, err := query.ListBatches(r.Context(), deviceID, limit, offset)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		writeJSON(w, http.StatusOK, manifests)
	})
}

func archivePage(r *http.Request) (int, int, error) {
	parse := func(name string) (int, error) {
		value := r.URL.Query().Get(name)
		if value == "" {
			return 0, nil
		}
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 0 {
			return 0, errors.New("invalid page")
		}
		return parsed, nil
	}
	limit, err := parse("limit")
	if err != nil {
		return 0, 0, err
	}
	offset, err := parse("offset")
	if err != nil {
		return 0, 0, err
	}
	return limit, offset, nil
}
