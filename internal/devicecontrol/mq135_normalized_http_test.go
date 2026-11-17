package devicecontrol

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

func TestAuthenticatedMQ135HTTPPersistsExactMQG2AndMQG1Results(t *testing.T) {
	store, _, now := testStore(t)
	handler := NewHandler(store, HTTPOptions{Operator: operatorAuth{allow: true}, Device: deviceAuth{allow: true}})
	for _, fixture := range []struct {
		id   string
		path string
	}{
		{"mqg2-in-range", "../../contracts/calibration/mq135-nh3/v2/fixtures/in-range.json"},
		{"mqg1-legacy", "../../contracts/calibration/mq135-nh3/v2/fixtures/legacy-mqg1.json"},
	} {
		t.Run(fixture.id, func(t *testing.T) {
			payload, err := os.ReadFile(fixture.path)
			if err != nil {
				t.Fatal(err)
			}
			request, err := json.Marshal(CreateRequest{
				ID: fixture.id, SensorID: "mq135_gas", Operation: "evidence",
				Payload: []byte(`{}`), ExpiresAt: now.Add(time.Minute),
			})
			if err != nil {
				t.Fatal(err)
			}
			created := httptest.NewRecorder()
			handler.ServeHTTP(created, httptest.NewRequest(http.MethodPost, "/api/device-control/commands/DEV-01", strings.NewReader(string(request))))
			if created.Code != http.StatusAccepted {
				t.Fatalf("operator create status=%d body=%s", created.Code, created.Body.String())
			}

			next := httptest.NewRecorder()
			handler.ServeHTTP(next, httptest.NewRequest(http.MethodGet, "/api/device-control/device/DEV-01/next", nil))
			if next.Code != http.StatusOK {
				t.Fatalf("authenticated device claim status=%d body=%s", next.Code, next.Body.String())
			}

			resultBytes, err := json.Marshal(ResultRequest{Status: Accepted, Payload: payload})
			if err != nil {
				t.Fatal(err)
			}
			posted := httptest.NewRecorder()
			handler.ServeHTTP(posted, httptest.NewRequest(http.MethodPost, "/api/device-control/device/DEV-01/"+fixture.id+"/result", strings.NewReader(string(resultBytes))))
			if posted.Code != http.StatusOK {
				t.Fatalf("authenticated result status=%d body=%s", posted.Code, posted.Body.String())
			}

			readback := httptest.NewRecorder()
			handler.ServeHTTP(readback, httptest.NewRequest(http.MethodGet, "/api/device-control/commands/DEV-01/"+fixture.id, nil))
			var response OperatorCommandResponse
			if readback.Code != http.StatusOK || json.Unmarshal(readback.Body.Bytes(), &response) != nil || !bytesEqual(response.Result, payload) {
				t.Fatalf("operator readback status=%d body=%s", readback.Code, readback.Body.String())
			}
			stored, err := store.Get(context.Background(), "DEV-01", fixture.id)
			if err != nil || stored.Status != Accepted || !bytesEqual(stored.ResultPayload, payload) {
				t.Fatalf("stored result changed: status=%s err=%v", stored.Status, err)
			}
		})
	}
}

func TestAuthenticatedMQ135HTTPRejectsInvalidResultWithoutStateMutation(t *testing.T) {
	store, _, now := testStore(t)
	handler := NewHandler(store, HTTPOptions{Operator: operatorAuth{allow: true}, Device: deviceAuth{allow: true}})
	request, err := json.Marshal(CreateRequest{
		ID: "mq135-invalid", SensorID: "mq135_gas", Operation: "evidence",
		Payload: []byte(`{}`), ExpiresAt: now.Add(time.Minute),
	})
	if err != nil {
		t.Fatal(err)
	}
	created := httptest.NewRecorder()
	handler.ServeHTTP(created, httptest.NewRequest(http.MethodPost, "/api/device-control/commands/DEV-01", strings.NewReader(string(request))))
	if created.Code != http.StatusAccepted {
		t.Fatalf("create status=%d body=%s", created.Code, created.Body.String())
	}
	next := httptest.NewRecorder()
	handler.ServeHTTP(next, httptest.NewRequest(http.MethodGet, "/api/device-control/device/DEV-01/next", nil))
	if next.Code != http.StatusOK {
		t.Fatalf("claim status=%d body=%s", next.Code, next.Body.String())
	}
	invalid, err := os.ReadFile("../../contracts/calibration/mq135-nh3/v2/fixtures/negative/serializer-stress-invalid.json")
	if err != nil {
		t.Fatal(err)
	}
	body, err := json.Marshal(ResultRequest{Status: Accepted, Payload: invalid})
	if err != nil {
		t.Fatal(err)
	}
	posted := httptest.NewRecorder()
	handler.ServeHTTP(posted, httptest.NewRequest(http.MethodPost, "/api/device-control/device/DEV-01/mq135-invalid/result", strings.NewReader(string(body))))
	if posted.Code != http.StatusBadRequest {
		t.Fatalf("invalid result status=%d body=%s", posted.Code, posted.Body.String())
	}
	stored, err := store.Get(context.Background(), "DEV-01", "mq135-invalid")
	if err != nil || stored.Status != Delivered || len(stored.ResultPayload) != 0 {
		t.Fatalf("rejected result changed command state: status=%s result=%q err=%v", stored.Status, stored.ResultPayload, err)
	}
}

func bytesEqual(left, right []byte) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		if left[index] != right[index] {
			return false
		}
	}
	return true
}

func TestAuthenticatedMQ135HTTPPersistsIdentityDiagnosticAndRejectsForgery(t *testing.T) {
	store, _, now := testStore(t)
	handler := NewHandler(store, HTTPOptions{Operator: operatorAuth{allow: true}, Device: deviceAuth{allow: true}})
	valid := mq135MQG2DiagnosticInput(t, "SOURCE_IDENTITY_INVALID", "INVALID", nil)
	forged := mutateMQ135MQG2Input(t, valid, func(root map[string]any) {
		root["response"].(map[string]any)["q"] = json.Number("0.29982668977469662")
	})
	for _, test := range []struct {
		id      string
		payload []byte
		status  int
	}{
		{id: "mq135-identity-diagnostic", payload: valid, status: http.StatusOK},
		{id: "mq135-identity-forgery", payload: forged, status: http.StatusBadRequest},
	} {
		t.Run(test.id, func(t *testing.T) {
			request, err := json.Marshal(CreateRequest{
				ID: test.id, SensorID: "mq135_gas", Operation: "evidence",
				Payload: []byte(`{}`), ExpiresAt: now.Add(time.Minute),
			})
			if err != nil {
				t.Fatal(err)
			}
			created := httptest.NewRecorder()
			handler.ServeHTTP(created, httptest.NewRequest(http.MethodPost, "/api/device-control/commands/DEV-01", strings.NewReader(string(request))))
			if created.Code != http.StatusAccepted {
				t.Fatalf("operator create status=%d body=%s", created.Code, created.Body.String())
			}
			next := httptest.NewRecorder()
			handler.ServeHTTP(next, httptest.NewRequest(http.MethodGet, "/api/device-control/device/DEV-01/next", nil))
			if next.Code != http.StatusOK {
				t.Fatalf("authenticated device claim status=%d body=%s", next.Code, next.Body.String())
			}
			resultBytes, err := json.Marshal(ResultRequest{Status: Accepted, Payload: test.payload})
			if err != nil {
				t.Fatal(err)
			}
			posted := httptest.NewRecorder()
			handler.ServeHTTP(posted, httptest.NewRequest(http.MethodPost, "/api/device-control/device/DEV-01/"+test.id+"/result", strings.NewReader(string(resultBytes))))
			if posted.Code != test.status {
				t.Fatalf("device result status=%d want=%d body=%s", posted.Code, test.status, posted.Body.String())
			}
			stored, err := store.Get(context.Background(), "DEV-01", test.id)
			if err != nil {
				t.Fatal(err)
			}
			if test.status == http.StatusOK {
				readback := httptest.NewRecorder()
				handler.ServeHTTP(readback, httptest.NewRequest(http.MethodGet, "/api/device-control/commands/DEV-01/"+test.id, nil))
				var response OperatorCommandResponse
				if readback.Code != http.StatusOK || json.Unmarshal(readback.Body.Bytes(), &response) != nil || !bytesEqual(response.Result, valid) {
					t.Fatalf("diagnostic readback status=%d body=%s", readback.Code, readback.Body.String())
				}
				if stored.Status != Accepted || !bytesEqual(stored.ResultPayload, valid) {
					t.Fatalf("accepted diagnostic was not persisted exactly: status=%s", stored.Status)
				}
			} else if stored.Status != Delivered || len(stored.ResultPayload) != 0 {
				t.Fatalf("rejected forgery changed command state: status=%s result=%q", stored.Status, stored.ResultPayload)
			}
		})
	}
}
