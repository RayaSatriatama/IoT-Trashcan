package devicecontrol

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

type operatorAuth struct{ allow bool }

func (a operatorAuth) AuthorizeOperator(context.Context, *http.Request, string) (string, error) {
	if !a.allow {
		return "", ErrUnauthorized
	}
	return "operator-1", nil
}

type deviceAuth struct{ allow bool }

func (a deviceAuth) AuthorizeDevice(context.Context, *http.Request, string) error {
	if !a.allow {
		return ErrUnauthorized
	}
	return nil
}

func TestHTTPRequiresAuthorizersAndSupportsOperatorDeviceFlow(t *testing.T) {
	s, _, now := testStore(t)
	h := NewHandler(s, HTTPOptions{Operator: operatorAuth{allow: true}, Device: deviceAuth{allow: true}, AllowedOrigin: "https://sigap.test"})
	req := httptest.NewRequest(http.MethodPost, "/api/device-control/commands/DEV-01", strings.NewReader(`{"command_id":"http-1","session_id":"session-http","sensor_id":"mq4","operation":"start","payload":{"reference":"fixture"},"expires_at":"2026-10-09T12:01:00Z"}`))
	req.Header.Set("Origin", "https://sigap.test")
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	if rr.Code != http.StatusAccepted {
		t.Fatalf("enqueue status=%d body=%s", rr.Code, rr.Body.String())
	}
	claim := httptest.NewRecorder()
	h.ServeHTTP(claim, httptest.NewRequest(http.MethodGet, "/api/device-control/device/DEV-01/next", nil))
	if claim.Code != http.StatusOK {
		t.Fatalf("claim status=%d", claim.Code)
	}
	var wire map[string]any
	if err := json.Unmarshal(claim.Body.Bytes(), &wire); err != nil {
		t.Fatal(err)
	}
	for _, field := range []string{"operator_id", "request_hash", "expires_in_ms"} {
		if _, ok := wire[field]; !ok {
			t.Fatalf("device wire missing %s: %s", field, claim.Body.String())
		}
	}
	result := httptest.NewRequest(http.MethodPost, "/api/device-control/device/DEV-01/http-1/result", strings.NewReader(`{"status":"ACCEPTED","session_id":"session-http","result":{"accepted":true}}`))
	out := httptest.NewRecorder()
	h.ServeHTTP(out, result)
	if out.Code != http.StatusOK {
		t.Fatalf("result status=%d body=%s", out.Code, out.Body.String())
	}
	got, err := s.Get(context.Background(), "DEV-01", "http-1")
	if err != nil || got.Status != Accepted {
		t.Fatalf("state=%+v err=%v", got, err)
	}
	_ = now
}

func TestHTTPRejectsOriginAndUnauthorizedDevice(t *testing.T) {
	s, _, _ := testStore(t)
	h := NewHandler(s, HTTPOptions{Operator: operatorAuth{allow: true}, Device: deviceAuth{allow: false}, AllowedOrigin: "https://sigap.test"})
	req := httptest.NewRequest(http.MethodPost, "/api/device-control/commands/DEV-01", strings.NewReader(`{"command_id":"x","sensor_id":"mq4","operation":"capture","payload":{},"expires_at":"2026-10-09T12:01:00Z"}`))
	req.Header.Set("Origin", "https://evil.test")
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	if rr.Code != http.StatusForbidden {
		t.Fatalf("origin status=%d", rr.Code)
	}
	res := httptest.NewRecorder()
	h.ServeHTTP(res, httptest.NewRequest(http.MethodGet, "/api/device-control/device/DEV-01/next", nil))
	if res.Code != http.StatusUnauthorized {
		t.Fatalf("device status=%d", res.Code)
	}
	_ = time.Now()
}

func TestHTTPDiagnosticsUsesOperatorAuthorizationAndExactResult(t *testing.T) {
	s, _, now := testStore(t)
	req := commandRequest(now)
	req.ID = "cmd-http-diagnostic"
	req.SensorID = "sht31_44"
	if _, err := s.Enqueue(context.Background(), req); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Claim(context.Background(), req.DeviceID); err != nil {
		t.Fatal(err)
	}
	const payload = `{"sensor_id":"sht31_44","quality":"UNCALIBRATED","raw":{"present":true}}`
	if _, err := s.UpdateResult(context.Background(), req.DeviceID, req.ID, ResultRequest{Status: Accepted, SessionID: req.SessionID, Payload: []byte(payload)}); err != nil {
		t.Fatal(err)
	}
	h := NewHandler(s, HTTPOptions{Operator: operatorAuth{allow: true}})
	r := httptest.NewRequest(http.MethodGet, "/api/device-control/commands/DEV-01/diagnostics?sensor_id=sht31_44", nil)
	res := httptest.NewRecorder()
	h.ServeHTTP(res, r)
	if res.Code != http.StatusOK || res.Body.String() != payload {
		t.Fatalf("status=%d body=%q", res.Code, res.Body.String())
	}
}

func TestHTTPCapabilitiesAreBoundedAndSensorSpecific(t *testing.T) {
	s, _, _ := testStore(t)
	h := NewHandler(s, HTTPOptions{Operator: operatorAuth{allow: true}})
	r := httptest.NewRequest(http.MethodGet,
		"/api/device-control/commands/DEV-01/capabilities?sensor_id=ds18b20_b3", nil)
	res := httptest.NewRecorder()
	h.ServeHTTP(res, r)
	if res.Code != http.StatusOK || !strings.Contains(res.Body.String(), `"sensor_id":"ds18b20_b3"`) ||
		!strings.Contains(res.Body.String(), `"unit":"centi_celsius"`) {
		t.Fatalf("status=%d body=%q", res.Code, res.Body.String())
	}
	unknown := httptest.NewRecorder()
	h.ServeHTTP(unknown, httptest.NewRequest(http.MethodGet,
		"/api/device-control/commands/DEV-01/capabilities?sensor_id=unknown", nil))
	if unknown.Code != http.StatusNotFound {
		t.Fatalf("unknown status=%d", unknown.Code)
	}
}

func TestHTTPCapabilitiesMatchFirmwareFamilies(t *testing.T) {
	s, _, _ := testStore(t)
	h := NewHandler(s, HTTPOptions{Operator: operatorAuth{allow: true}})
	for _, test := range []struct {
		sensor string
		fields []string
	}{
		{"hx711_organic", []string{"tare_counts", "reference_mass_mg", "max_span_counts"}},
		{"ultrasonic_b3", []string{"empty_mm", "full_mm", "echo_timeout_us", "inter_sensor_gap_us"}},
		{"mq7", []string{"reference_mv", "full_scale_counts", "divider_numerator", "divider_denominator"}},
		{"ds18b20_b3", []string{"reference_temperature_centi_c", "resolution_bits", "max_span_centi_c"}},
		{"sht31_45", []string{"reference_temperature_centi_c", "reference_humidity_centi_pct", "address"}},
	} {
		t.Run(test.sensor, func(t *testing.T) {
			res := httptest.NewRecorder()
			h.ServeHTTP(res, httptest.NewRequest(http.MethodGet,
				"/api/device-control/commands/DEV-01/capabilities?sensor_id="+test.sensor, nil))
			if res.Code != http.StatusOK {
				t.Fatalf("status=%d body=%q", res.Code, res.Body.String())
			}
			for _, field := range test.fields {
				if !strings.Contains(res.Body.String(), `"name":"`+field+`"`) {
					t.Fatalf("missing field %q: %s", field, res.Body.String())
				}
			}
		})
	}
}

func TestHTTPRejectsEnvelopeOverFirmwareFourKiBBound(t *testing.T) {
	if MaxPayloadBytes != 4*1024 {
		t.Fatalf("device-control bound=%d, firmware payload bound is 4096", MaxPayloadBytes)
	}
	s, _, _ := testStore(t)
	h := NewHandler(s, HTTPOptions{Operator: operatorAuth{allow: true}})
	oversized := `{"command_id":"too-large","session_id":"session","sensor_id":"mq4","operation":"start","payload":"` + strings.Repeat("x", MaxPayloadBytes) + `","expires_at":"2026-10-09T12:01:00Z"}`
	res := httptest.NewRecorder()
	h.ServeHTTP(res, httptest.NewRequest(http.MethodPost,
		"/api/device-control/commands/DEV-01", strings.NewReader(oversized)))
	if res.Code != http.StatusBadRequest {
		t.Fatalf("status=%d body=%s", res.Code, res.Body.String())
	}
}
