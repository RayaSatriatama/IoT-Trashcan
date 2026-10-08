package httpapi

import (
	"context"
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

type telemetryRecordQuery interface {
	ListRecords(context.Context, telemetryarchive.RecordQuery) (telemetryarchive.RecordPage, error)
	ExportRecordsJSON(context.Context, telemetryarchive.RecordQuery, io.Writer) error
}

func newTelemetryRecordArchiveHandler(query telemetryRecordQuery, authorizer telemetryArchiveAuthorizer) http.Handler {
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
		request, err := archiveRecordQuery(r, deviceID)
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid archive record query")
			return
		}
		if strings.HasSuffix(r.URL.Path, "/export") {
			if err := telemetryarchive.ValidateRecordQuery(request); err != nil {
				writeArchiveRecordQueryError(w, err)
				return
			}
			preflight := request
			preflight.Limit = 1
			if _, err := query.ListRecords(r.Context(), preflight); err != nil {
				writeArchiveRecordQueryError(w, err)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusOK)
			if err := query.ExportRecordsJSON(r.Context(), request, w); err != nil {
				return
			}
			return
		}
		page, err := query.ListRecords(r.Context(), request)
		if err != nil {
			writeArchiveRecordQueryError(w, err)
			return
		}
		writeJSON(w, http.StatusOK, page)
	})
}

func writeArchiveRecordQueryError(w http.ResponseWriter, err error) {
	if errors.Is(err, telemetryarchive.ErrInvalidQuery) {
		writeError(w, http.StatusBadRequest, "invalid archive record query")
	} else {
		writeError(w, http.StatusInternalServerError, "internal server error")
	}
}

func archiveRecordQuery(r *http.Request, deviceID string) (telemetryarchive.RecordQuery, error) {
	values := r.URL.Query()
	request := telemetryarchive.RecordQuery{
		DeviceID: deviceID,
		Cursor:   values.Get("cursor"),
		Kind:     values.Get("kind"),
		Stream:   values.Get("stream"),
		SensorID: values.Get("sensor_id"),
		Quality:  values.Get("quality"),
	}
	if value := values.Get("limit"); value != "" {
		limit, err := strconv.Atoi(value)
		if err != nil || limit < 0 {
			return telemetryarchive.RecordQuery{}, errors.New("invalid limit")
		}
		request.Limit = limit
	}
	if value := values.Get("utc_null"); value != "" {
		parsed, err := strconv.ParseBool(value)
		if err != nil {
			return telemetryarchive.RecordQuery{}, errors.New("invalid utc_null")
		}
		request.UTCNull = &parsed
	}
	return request, nil
}
