package httpapi

import (
	"context"
	"errors"
	"net/http"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

type telemetryCurrentQuery interface {
	Current(context.Context, string) (telemetryarchive.CurrentSnapshot, error)
}

func newTelemetryCurrentHandler(query telemetryCurrentQuery, authorizer telemetryArchiveAuthorizer) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		if query == nil || authorizer == nil {
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		token, ok := bearerToken(r.Header.Get("Authorization"))
		if !ok {
			writeError(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		deviceID := r.PathValue("device_id")
		if err := authorizer.AuthorizeDevice(r.Context(), deviceID, token); err != nil {
			if errors.Is(err, telemetryarchive.ErrUnauthorized) {
				writeError(w, http.StatusUnauthorized, "unauthorized")
			} else {
				writeError(w, http.StatusInternalServerError, "internal server error")
			}
			return
		}
		snapshot, err := query.Current(r.Context(), deviceID)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		writeJSON(w, http.StatusOK, snapshot)
	})
}
