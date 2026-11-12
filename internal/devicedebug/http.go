package devicedebug

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
)

const httpMaxBody = 4096

type HTTPOptions struct {
	Operator      devicecontrol.OperatorAuthorizer
	Device        devicecontrol.DeviceAuthorizer
	AllowedOrigin string
}
type httpHandler struct {
	store   *Store
	options HTTPOptions
}

func NewHandler(store *Store, options HTTPOptions) http.Handler {
	return &httpHandler{store: store, options: options}
}

func (h *httpHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if h == nil || h.store == nil {
		debugError(w, 500, "internal server error")
		return
	}
	// Split before unescaping so an encoded slash remains part of one identifier.
	parts := strings.Split(strings.TrimPrefix(r.URL.EscapedPath(), "/api/device-debug/"), "/")
	for i, part := range parts {
		decoded, err := url.PathUnescape(part)
		if err != nil {
			debugError(w, http.StatusBadRequest, "invalid path")
			return
		}
		parts[i] = decoded
	}
	if len(parts) == 3 && parts[0] == "device" && parts[2] == "events" && r.Method == http.MethodPost {
		h.upload(w, r, parts[1])
		return
	}
	if len(parts) == 2 && parts[0] == "events" && r.Method == http.MethodGet {
		h.history(w, r, parts[1])
		return
	}
	if len(parts) < 2 {
		debugError(w, 404, "not found")
		return
	}
	debugError(w, 405, "method not allowed")
}
func (h *httpHandler) upload(w http.ResponseWriter, r *http.Request, deviceID string) {
	if h.options.Device == nil || h.options.Device.AuthorizeDevice(r.Context(), r, deviceID) != nil {
		debugError(w, 401, "unauthorized")
		return
	}
	if !h.allowOrigin(w, r) {
		return
	}
	raw, err := io.ReadAll(io.LimitReader(r.Body, httpMaxBody+1))
	if err != nil {
		debugError(w, 400, "invalid upload")
		return
	}
	if len(raw) > httpMaxBody {
		debugError(w, 413, "payload too large")
		return
	}
	upload, err := DecodeUpload(raw)
	if err != nil || upload.DeviceID != deviceID {
		debugError(w, 400, "invalid upload")
		return
	}
	ack, err := h.store.Ingest(r.Context(), upload, raw)
	if errors.Is(err, ErrConflict) {
		debugError(w, 409, "content conflict")
		return
	}
	if errors.Is(err, ErrInvalid) {
		debugError(w, 400, "invalid upload")
		return
	}
	if errors.Is(err, ErrNotFound) {
		debugError(w, 404, "not found")
		return
	}
	if err != nil {
		debugError(w, 500, "internal server error")
		return
	}
	debugJSON(w, 200, ack)
}
func (h *httpHandler) history(w http.ResponseWriter, r *http.Request, deviceID string) {
	if !h.allowOrigin(w, r) {
		return
	}
	if h.options.Operator == nil {
		debugError(w, 401, "unauthorized")
		return
	}
	operator, err := h.options.Operator.AuthorizeOperator(r.Context(), r, deviceID)
	if err != nil || operator == "" {
		debugError(w, 401, "unauthorized")
		return
	}
	query, err := url.ParseQuery(r.URL.RawQuery)
	if err != nil {
		debugError(w, 400, "invalid query")
		return
	}
	for key, values := range query {
		if (key != "after" && key != "limit") || len(values) != 1 {
			debugError(w, 400, "invalid query")
			return
		}
	}
	after, limit := "0", 100
	if values, ok := query["after"]; ok {
		after = values[0]
	}
	if values, ok := query["limit"]; ok {
		value := values[0]
		if value == "" || (len(value) > 1 && value[0] == '0') {
			debugError(w, 400, "invalid query")
			return
		}
		for i := range value {
			if value[i] < '0' || value[i] > '9' {
				debugError(w, 400, "invalid query")
				return
			}
		}
		n, err := strconv.Atoi(value)
		if err != nil || n < 1 || n > 100 {
			debugError(w, 400, "invalid query")
			return
		}
		limit = n
	}
	page, err := h.store.Query(r.Context(), deviceID, after, limit)
	if errors.Is(err, ErrInvalid) {
		debugError(w, 400, "invalid query")
		return
	}
	if errors.Is(err, ErrNotFound) {
		debugError(w, 404, "not found")
		return
	}
	if err != nil {
		debugError(w, 500, "internal server error")
		return
	}
	debugJSON(w, 200, page)
}
func (h *httpHandler) allowOrigin(w http.ResponseWriter, r *http.Request) bool {
	if origin := r.Header.Get("Origin"); origin != "" && h.options.AllowedOrigin != "" && origin != h.options.AllowedOrigin {
		debugError(w, 403, "origin not allowed")
		return false
	}
	return true
}
func debugError(w http.ResponseWriter, status int, message string) {
	debugJSON(w, status, map[string]string{"error": message})
}
func debugJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
