package devicecontrol

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
)

// OperatorAuthorizer is deliberately injected.  A nil authorizer never
// exposes command endpoints, preventing accidental unauthenticated deployment.
type OperatorAuthorizer interface {
	AuthorizeOperator(context.Context, *http.Request, string) (string, error)
}
type DeviceAuthorizer interface {
	AuthorizeDevice(context.Context, *http.Request, string) error
}
type HTTPOptions struct {
	Operator      OperatorAuthorizer
	Device        DeviceAuthorizer
	AllowedOrigin string
}

type Handler struct {
	store   *Store
	options HTTPOptions
}

func NewHandler(store *Store, options HTTPOptions) http.Handler {
	return &Handler{store: store, options: options}
}

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if h == nil || h.store == nil {
		writeControlError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	path := strings.TrimPrefix(r.URL.Path, "/api/device-control/")
	parts := strings.Split(strings.Trim(path, "/"), "/")
	if len(parts) < 2 {
		writeControlError(w, http.StatusNotFound, "not found")
		return
	}
	if parts[0] == "commands" {
		h.operator(w, r, parts[1], parts[2:])
		return
	}
	if parts[0] == "device" {
		h.device(w, r, parts[1], parts[2:])
		return
	}
	writeControlError(w, http.StatusNotFound, "not found")
}

func (h *Handler) operator(w http.ResponseWriter, r *http.Request, deviceID string, parts []string) {
	if h.options.Operator == nil {
		writeControlError(w, http.StatusUnauthorized, "operator authorization unavailable")
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" && h.options.AllowedOrigin != "" && origin != h.options.AllowedOrigin {
		writeControlError(w, http.StatusForbidden, "origin not allowed")
		return
	}
	operator, err := h.options.Operator.AuthorizeOperator(r.Context(), r, deviceID)
	if err != nil || operator == "" {
		writeControlError(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if len(parts) == 0 && r.Method == http.MethodPost {
		var req CreateRequest
		if err := decodeBoundedJSON(r, &req); err != nil {
			writeControlError(w, http.StatusBadRequest, "invalid command")
			return
		}
		req.DeviceID = deviceID
		req.OperatorID = operator
		c, err := h.store.Enqueue(r.Context(), req)
		if errors.Is(err, ErrConflict) || errors.Is(err, ErrSessionBusy) {
			writeControlError(w, http.StatusConflict, err.Error())
			return
		}
		if errors.Is(err, ErrExpired) {
			writeControlError(w, http.StatusGone, "command expired")
			return
		}
		if err != nil {
			writeControlError(w, http.StatusBadRequest, "invalid command")
			return
		}
		writeControlJSON(w, http.StatusAccepted, c)
		return
	}
	if len(parts) == 0 {
		writeControlError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	commandID := parts[0]
	if commandID == "diagnostics" && r.Method == http.MethodGet {
		sensorID := strings.TrimSpace(r.URL.Query().Get("sensor_id"))
		payload, err := h.store.LatestResult(r.Context(), deviceID, sensorID)
		if errors.Is(err, ErrNotFound) {
			writeControlError(w, http.StatusNotFound, "diagnostic unavailable")
			return
		}
		if err != nil {
			writeControlError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(payload)
		return
	}
	if commandID == "capabilities" && r.Method == http.MethodGet {
		sensorID := strings.TrimSpace(r.URL.Query().Get("sensor_id"))
		capability, ok := sensorCapability(sensorID)
		if !ok {
			writeControlError(w, http.StatusNotFound, "capabilities unavailable")
			return
		}
		writeControlJSON(w, http.StatusOK, map[string]any{"capabilities": capability})
		return
	}
	if len(parts) == 1 && r.Method == http.MethodGet {
		c, err := h.store.Get(r.Context(), deviceID, commandID)
		if errors.Is(err, ErrNotFound) {
			writeControlError(w, http.StatusNotFound, "not found")
			return
		}
		if err != nil {
			writeControlError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		writeControlJSON(w, http.StatusOK, c)
		return
	}
	if len(parts) == 1 && r.Method == http.MethodDelete {
		if err := h.store.Cancel(r.Context(), deviceID, commandID); err != nil {
			writeControlError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeControlError(w, http.StatusMethodNotAllowed, "method not allowed")
}

type capabilityField struct {
	Name     string `json:"name"`
	Unit     string `json:"unit"`
	Required bool   `json:"required"`
}

type sensorCapabilityResponse struct {
	SensorID              string            `json:"sensor_id"`
	References            []capabilityField `json:"references"`
	Criteria              []capabilityField `json:"criteria"`
	EngineeringParameters []capabilityField `json:"engineering_parameters"`
}

func sensorCapability(sensorID string) (sensorCapabilityResponse, bool) {
	if sensorID == "" {
		return sensorCapabilityResponse{}, false
	}
	stream := map[string]string{
		"hx711_organic": "kg", "hx711_inorganic": "kg", "hx711_b3": "kg",
		"ultrasonic_organic": "mm", "ultrasonic_inorganic": "mm", "ultrasonic_b3": "mm",
		"mq4": "mV", "mq135": "mV", "mq7": "mV", "mq8": "mV",
		"ds18b20_b3": "degC", "sht31_44": "degC/%RH", "sht31_45": "degC/%RH",
	}
	unit, ok := stream[sensorID]
	if !ok {
		return sensorCapabilityResponse{}, false
	}
	return sensorCapabilityResponse{
		SensorID:              sensorID,
		References:            []capabilityField{{Name: "primary", Unit: unit, Required: true}},
		Criteria:              []capabilityField{{Name: "stability_window", Unit: "samples", Required: true}},
		EngineeringParameters: []capabilityField{{Name: "sensor_specific", Unit: "typed_profile", Required: true}},
	}, true
}

func (h *Handler) device(w http.ResponseWriter, r *http.Request, deviceID string, parts []string) {
	if h.options.Device == nil || h.options.Device.AuthorizeDevice(r.Context(), r, deviceID) != nil {
		writeControlError(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if len(parts) == 1 && parts[0] == "next" && r.Method == http.MethodGet {
		c, err := h.store.Claim(r.Context(), deviceID)
		if errors.Is(err, ErrNotFound) {
			writeControlError(w, http.StatusNoContent, "")
			return
		}
		if err != nil {
			writeControlError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		writeControlJSON(w, http.StatusOK, c)
		return
	}
	if len(parts) == 2 && parts[1] == "result" && r.Method == http.MethodPost {
		var result ResultRequest
		if err := decodeBoundedJSON(r, &result); err != nil {
			writeControlError(w, http.StatusBadRequest, "invalid result")
			return
		}
		c, err := h.store.UpdateResult(r.Context(), deviceID, parts[0], result)
		if errors.Is(err, ErrExpired) {
			writeControlError(w, http.StatusGone, "command expired")
			return
		}
		if errors.Is(err, ErrConflict) {
			writeControlError(w, http.StatusConflict, "command conflict")
			return
		}
		if err != nil {
			writeControlError(w, http.StatusBadRequest, "invalid result")
			return
		}
		writeControlJSON(w, http.StatusOK, c)
		return
	}
	writeControlError(w, http.StatusNotFound, "not found")
}

func decodeBoundedJSON(r *http.Request, dst any) error {
	raw, err := io.ReadAll(io.LimitReader(r.Body, MaxPayloadBytes+1))
	if err != nil || len(raw) > MaxPayloadBytes {
		return errors.New("payload too large")
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	if err = dec.Decode(dst); err != nil {
		return err
	}
	var extra any
	if err = dec.Decode(&extra); err != io.EOF {
		return errors.New("trailing json")
	}
	return nil
}
func writeControlError(w http.ResponseWriter, status int, msg string) {
	if status == http.StatusNoContent {
		w.WriteHeader(status)
		return
	}
	writeControlJSON(w, status, map[string]string{"error": msg})
}
func writeControlJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
