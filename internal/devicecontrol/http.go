package devicecontrol

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
	"time"
)

// OperatorAuthorizer is deliberately injected.  A nil authorizer never
// exposes command endpoints, preventing accidental unauthenticated deployment.
type OperatorAuthorizer interface {
	AuthorizeOperator(context.Context, *http.Request, string) (string, error)
}
type DeviceAuthorizer interface {
	AuthorizeDevice(context.Context, *http.Request, string) error
}
type DiagnosticSource interface {
	LatestDiagnostic(context.Context, string, string) (json.RawMessage, error)
}
type HTTPOptions struct {
	Operator      OperatorAuthorizer
	Device        DeviceAuthorizer
	Diagnostics   DiagnosticSource
	AllowedOrigin string
}

// OperatorCommandResponse is deliberately separate from deviceCommandWire.
// The former is a durable readback contract for the operator UI; the latter
// is the bounded delivery envelope sent to firmware.
type OperatorCommandResponse struct {
	CommandID string          `json:"command_id"`
	DeviceID  string          `json:"device_id"`
	SensorID  string          `json:"sensor_id"`
	Operation string          `json:"operation"`
	SessionID string          `json:"session_id,omitempty"`
	Status    Status          `json:"status"`
	Result    json.RawMessage `json:"result,omitempty"`
	ErrorCode string          `json:"error_code,omitempty"`
	ExpiresAt time.Time       `json:"expires_at"`
	Attempt   int             `json:"attempt"`
	CreatedAt time.Time       `json:"created_at"`
	UpdatedAt time.Time       `json:"updated_at"`
}

func commandForOperator(c Command) OperatorCommandResponse {
	return OperatorCommandResponse{
		CommandID: c.ID, DeviceID: c.DeviceID, SensorID: c.SensorID,
		Operation: c.Operation, SessionID: c.SessionID, Status: c.Status,
		Result: append(json.RawMessage(nil), c.ResultPayload...), ErrorCode: c.ErrorCode,
		ExpiresAt: c.ExpiresAt, Attempt: c.Attempt, CreatedAt: c.CreatedAt,
		UpdatedAt: c.UpdatedAt,
	}
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
		if h.options.Diagnostics == nil {
			writeControlError(w, http.StatusNotFound, "diagnostic unavailable")
			return
		}
		payload, err := h.options.Diagnostics.LatestDiagnostic(r.Context(), deviceID, sensorID)
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
		// A device permission permits monitoring of the device, but not
		// readback of another operator's calibration command or result.
		// Keeping the response indistinguishable from an unknown command avoids
		// revealing command existence or calibration provenance across sessions.
		if c.OperatorID != operator {
			writeControlError(w, http.StatusNotFound, "not found")
			return
		}
		writeControlJSON(w, http.StatusOK, commandForOperator(c))
		return
	}
	if len(parts) == 1 && r.Method == http.MethodDelete {
		if err := h.store.Cancel(r.Context(), deviceID, commandID, operator); err != nil {
			if errors.Is(err, ErrNotFound) {
				writeControlError(w, http.StatusNotFound, "not found")
				return
			}
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
	value := sensorCapabilityResponse{SensorID: sensorID}
	switch sensorID {
	case "hx711_organic", "hx711_inorganic", "hx711_b3":
		value.References = []capabilityField{{"tare_counts", "counts", true}, {"reference_mass_mg", "mg", true}}
		value.Criteria = []capabilityField{{"samples", "count", true}, {"max_span_counts", "counts", true}}
	case "ultrasonic_organic", "ultrasonic_inorganic", "ultrasonic_b3":
		value.References = []capabilityField{{"empty_mm", "mm", true}, {"full_mm", "mm", true}}
		value.Criteria = []capabilityField{{"samples", "count", true}, {"max_span_mm", "mm", true}}
		value.EngineeringParameters = []capabilityField{
			{"echo_mm_per_us_milli", "mm_per_us", true}, {"echo_timeout_us", "us", true},
			{"inter_sensor_gap_us", "us", true}, {"overshoot_tolerance_mm_milli", "mm", true},
		}
	case "mq4", "mq135", "mq7", "mq8":
		value.Criteria = []capabilityField{{"samples", "count", true}, {"max_span_counts", "counts", true}}
		value.EngineeringParameters = []capabilityField{
			{"reference_mv", "mV", true}, {"full_scale_counts", "counts", true},
			{"divider_numerator", "ratio", true}, {"divider_denominator", "ratio", true},
		}
	case "ds18b20_b3":
		value.References = []capabilityField{{"reference_temperature_centi_c", "centi_celsius", true}}
		value.Criteria = []capabilityField{{"samples", "count", true}, {"max_span_centi_c", "centi_celsius", true}}
		value.EngineeringParameters = []capabilityField{{"resolution_bits", "bits", true}}
	case "sht31_44", "sht31_45":
		value.References = []capabilityField{{"reference_temperature_centi_c", "centi_celsius", true}, {"reference_humidity_centi_pct", "centi_percent", true}}
		value.Criteria = []capabilityField{{"samples", "count", true}, {"max_temperature_span_centi_c", "centi_celsius", true}, {"max_humidity_span_centi_pct", "centi_percent", true}}
		value.EngineeringParameters = []capabilityField{{"address", "i2c", true}}
	default:
		return sensorCapabilityResponse{}, false
	}
	return value, true
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
		writeControlJSON(w, http.StatusOK, commandForDevice(c, h.store.now().UTC()))
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
	if err = rejectDuplicateJSONKeys(raw); err != nil {
		return err
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err = dec.Decode(dst); err != nil {
		return err
	}
	var extra any
	if err = dec.Decode(&extra); err != io.EOF {
		return errors.New("trailing json")
	}
	return nil
}

// rejectDuplicateJSONKeys validates every object before it reaches an
// envelope struct. encoding/json otherwise accepts duplicate member names and
// silently chooses the final value, which makes signed command identity
// ambiguous across parsers.
func rejectDuplicateJSONKeys(raw []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	if err := consumeJSONValue(decoder, 0); err != nil {
		return err
	}
	if _, err := decoder.Token(); err != io.EOF {
		if err == nil {
			return errors.New("trailing json")
		}
		return err
	}
	return nil
}

func consumeJSONValue(decoder *json.Decoder, depth int) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, isDelimiter := token.(json.Delim)
	if !isDelimiter {
		return nil
	}
	if depth >= maxJSONNesting {
		return errors.New("json nesting exceeds bound")
	}
	switch delimiter {
	case '{':
		keys := make(map[string]struct{})
		for decoder.More() {
			keyToken, keyErr := decoder.Token()
			if keyErr != nil {
				return keyErr
			}
			key, ok := keyToken.(string)
			if !ok {
				return errors.New("object key is not a string")
			}
			if _, exists := keys[key]; exists {
				return errors.New("duplicate json key")
			}
			keys[key] = struct{}{}
			if err := consumeJSONValue(decoder, depth+1); err != nil {
				return err
			}
		}
		end, endErr := decoder.Token()
		if endErr != nil {
			return endErr
		}
		if value, ok := end.(json.Delim); !ok || value != '}' {
			return errors.New("invalid object")
		}
	case '[':
		for decoder.More() {
			if err := consumeJSONValue(decoder, depth+1); err != nil {
				return err
			}
		}
		end, endErr := decoder.Token()
		if endErr != nil {
			return endErr
		}
		if value, ok := end.(json.Delim); !ok || value != ']' {
			return errors.New("invalid array")
		}
	default:
		return errors.New("unexpected json delimiter")
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
