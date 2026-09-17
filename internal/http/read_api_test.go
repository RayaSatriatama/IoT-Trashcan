package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
)

func TestReadAPIOverviewPrioritizesActionableLocations(t *testing.T) {
	api, _ := readAPIFixture(t)
	request := httptest.NewRequest(http.MethodGet, "/api/overview", nil)
	response := httptest.NewRecorder()

	api.Overview(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body struct {
		TotalLocations    int `json:"total_locations"`
		TotalDevices      int `json:"total_devices"`
		PickupRequired    int `json:"pickup_required"`
		SafetyAlerts      int `json:"safety_alerts"`
		PriorityLocations []struct {
			ID string `json:"id"`
		} `json:"priority_locations"`
	}
	decodeBody(t, response, &body)
	if body.TotalLocations != 2 || body.TotalDevices != 2 {
		t.Fatalf("KPI locations=%d devices=%d", body.TotalLocations, body.TotalDevices)
	}
	if body.SafetyAlerts != 1 || body.PickupRequired != 0 {
		t.Fatalf("KPI safety=%d pickup=%d", body.SafetyAlerts, body.PickupRequired)
	}
	if len(body.PriorityLocations) != 1 || body.PriorityLocations[0].ID != "location-a" {
		t.Fatalf("priority locations=%+v", body.PriorityLocations)
	}
}

func TestReadAPIOverviewCountsDeviceKPIsIndependentlyOfLocationAggregate(t *testing.T) {
	api, db := mixedOverviewFixture(t)
	defer db.Close()
	request := httptest.NewRequest(http.MethodGet, "/api/overview", nil)
	response := httptest.NewRecorder()

	api.Overview(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body struct {
		TotalLocations int `json:"total_locations"`
		TotalDevices   int `json:"total_devices"`
		PickupRequired int `json:"pickup_required"`
		SafetyAlerts   int `json:"safety_alerts"`
		Priority       []struct {
			ID     string  `json:"id"`
			Status *string `json:"status"`
		} `json:"priority_locations"`
	}
	decodeBody(t, response, &body)
	if body.TotalLocations != 1 || body.TotalDevices != 2 || body.SafetyAlerts != 1 || body.PickupRequired != 1 {
		t.Fatalf("mixed KPI locations=%d devices=%d safety=%d pickup=%d", body.TotalLocations, body.TotalDevices, body.SafetyAlerts, body.PickupRequired)
	}
	if len(body.Priority) != 1 || body.Priority[0].ID != "location-mixed" || body.Priority[0].Status == nil || *body.Priority[0].Status != "Safety Alert" {
		t.Fatalf("mixed priority locations=%+v", body.Priority)
	}
}

func TestReadAPIOverviewLimitsRecentActivityToNewestThree(t *testing.T) {
	api, db := readAPIFixture(t)
	for index := 0; index < 5; index++ {
		startedAt := time.Date(2026, 9, 17, 9, 0, index, 0, time.UTC).Format(time.RFC3339)
		execFixture(t, db, `INSERT INTO events (device_id, event_type, severity, started_at) VALUES ('device-a', ?, 'info', ?)`, fmt.Sprintf("event-%d", index+1), startedAt)
	}
	request := httptest.NewRequest(http.MethodGet, "/api/overview", nil)
	response := httptest.NewRecorder()

	api.Overview(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body struct {
		RecentActivity []struct {
			EventType string `json:"event_type"`
		} `json:"recent_activity"`
	}
	decodeBody(t, response, &body)
	if len(body.RecentActivity) != 3 {
		t.Fatalf("recent activity count=%d, want 3", len(body.RecentActivity))
	}
	for index, want := range []string{"event-5", "event-4", "event-3"} {
		if body.RecentActivity[index].EventType != want {
			t.Fatalf("recent activity[%d]=%q, want %q", index, body.RecentActivity[index].EventType, want)
		}
	}
}

func TestReadAPILocationsSupportsSearchAndStatusFilter(t *testing.T) {
	api, _ := readAPIFixture(t)
	request := httptest.NewRequest(http.MethodGet, "/api/locations?q=alpha&status=Safety%20Alert", nil)
	response := httptest.NewRecorder()

	api.Locations(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body struct {
		Locations []struct {
			ID     string  `json:"id"`
			Status *string `json:"status"`
		} `json:"locations"`
	}
	decodeBody(t, response, &body)
	if len(body.Locations) != 1 || body.Locations[0].ID != "location-a" || body.Locations[0].Status == nil || *body.Locations[0].Status != "Safety Alert" {
		t.Fatalf("locations=%+v", body.Locations)
	}

	invalidRequest := httptest.NewRequest(http.MethodGet, "/api/locations?status=Unknown", nil)
	invalidResponse := httptest.NewRecorder()
	api.Locations(invalidResponse, invalidRequest)
	if invalidResponse.Code != http.StatusBadRequest {
		t.Fatalf("invalid status=%d body=%s", invalidResponse.Code, invalidResponse.Body.String())
	}
}

func TestReadAPILocationAndDeviceDetailsDoNotExposeCredential(t *testing.T) {
	api, _ := readAPIFixture(t)
	request := httptest.NewRequest(http.MethodGet, "/api/locations/location-a", nil)
	request.SetPathValue("id", "location-a")
	response := httptest.NewRecorder()

	api.Location(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	if strings.Contains(response.Body.String(), "credential_digest") || strings.Contains(response.Body.String(), "fixture-secret") {
		t.Fatalf("location response leaked credential: %s", response.Body.String())
	}
	var location struct {
		Status  *string `json:"status"`
		Devices []struct {
			ID    string `json:"id"`
			Slots []struct {
				Stream            string   `json:"stream"`
				WeightUtilization *float64 `json:"weight_utilization"`
				Status            *string  `json:"status"`
			} `json:"slots"`
		} `json:"devices"`
	}
	decodeBody(t, response, &location)
	if location.Status == nil || *location.Status != "Safety Alert" || len(location.Devices) != 1 || len(location.Devices[0].Slots) != 3 {
		t.Fatalf("location detail=%+v", location)
	}
	if location.Devices[0].Slots[0].WeightUtilization == nil || *location.Devices[0].Slots[0].WeightUtilization != 50 {
		t.Fatalf("slot normalization=%+v", location.Devices[0].Slots[0])
	}

	deviceRequest := httptest.NewRequest(http.MethodGet, "/api/devices/device-a", nil)
	deviceRequest.SetPathValue("id", "device-a")
	deviceResponse := httptest.NewRecorder()
	api.Device(deviceResponse, deviceRequest)
	if deviceResponse.Code != http.StatusOK {
		t.Fatalf("device status=%d body=%s", deviceResponse.Code, deviceResponse.Body.String())
	}
	if strings.Contains(deviceResponse.Body.String(), "fixture-secret") || strings.Contains(deviceResponse.Body.String(), "raw_payload") {
		t.Fatalf("device response leaked internal data: %s", deviceResponse.Body.String())
	}
}

func TestReadAPIResearchExposesRawAndNormalizedAuditableValues(t *testing.T) {
	api, _ := readAPIFixture(t)
	request := httptest.NewRequest(http.MethodGet, "/api/devices/device-a/research", nil)
	request.SetPathValue("id", "device-a")
	response := httptest.NewRecorder()

	api.Research(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	if strings.Contains(response.Body.String(), "ppm") || strings.Contains(response.Body.String(), "fixture-secret") {
		t.Fatalf("research response leaked or mislabeled raw gas: %s", response.Body.String())
	}
	var body struct {
		Slots []struct {
			Stream string `json:"stream"`
			Raw    struct {
				GasRaw *float64 `json:"gas_raw"`
			} `json:"raw"`
			Normalized struct {
				OGI               *float64 `json:"ogi"`
				AccumulationHours *float64 `json:"accumulation_hours"`
			} `json:"normalized"`
			FISScore *float64 `json:"fis_score"`
		} `json:"slots"`
	}
	decodeBody(t, response, &body)
	if len(body.Slots) != 3 || body.Slots[0].Raw.GasRaw == nil || *body.Slots[0].Raw.GasRaw != 42 {
		t.Fatalf("research slots=%+v", body.Slots)
	}
	if body.Slots[0].Normalized.OGI != nil || body.Slots[0].Normalized.AccumulationHours != nil {
		t.Fatalf("unavailable normalized values must remain null: %+v", body.Slots[0].Normalized)
	}
}

func TestReadAPINotFound(t *testing.T) {
	api, _ := readAPIFixture(t)
	request := httptest.NewRequest(http.MethodGet, "/api/devices/missing", nil)
	request.SetPathValue("id", "missing")
	response := httptest.NewRecorder()

	api.Device(response, request)

	if response.Code != http.StatusNotFound {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
}

func TestReadAPIDerivesOfflineWithoutConflatingDataInvalid(t *testing.T) {
	api, _ := readAPIFixture(t)
	request := httptest.NewRequest(http.MethodGet, "/api/devices/device-b", nil)
	request.SetPathValue("id", "device-b")
	response := httptest.NewRecorder()

	api.Device(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body struct {
		OnlineState *string `json:"online_state"`
		Status      *string `json:"status"`
	}
	decodeBody(t, response, &body)
	if body.OnlineState == nil || *body.OnlineState != "Offline" || body.Status == nil || *body.Status != "Offline" {
		t.Fatalf("offline derivation=%+v", body)
	}
}

func TestReadAPISupportsSingleConnectionPool(t *testing.T) {
	api, db := readAPIFixture(t)
	db.SetMaxOpenConns(1)
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	request := httptest.NewRequest(http.MethodGet, "/api/locations", nil).WithContext(ctx)
	response := httptest.NewRecorder()

	api.Locations(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
}

func TestRouterRegistersReadAPIRoutes(t *testing.T) {
	api, db := readAPIFixture(t)
	defer db.Close()
	handler := NewRouter(&fakeIngestService{}, WithReadAPI(api))
	request := httptest.NewRequest(http.MethodGet, "/api/devices/device-a", nil)
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
}

func readAPIFixture(t *testing.T) (*ReadAPI, *sql.DB) {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })

	execFixture(t, db, `INSERT INTO locations (id, name, address_text) VALUES ('location-a', 'Alpha Lab', 'Campus A'), ('location-b', 'Beta Lab', 'Campus B')`)
	execFixture(t, db, `INSERT INTO devices (id, location_id, device_code, placement_label, last_seen, credential_digest) VALUES
		('device-a', 'location-a', 'SIGAP-A', 'North gate', '2026-09-17T09:59:45Z', 'fixture-secret'),
		('device-b', 'location-b', 'SIGAP-B', 'South gate', '2026-09-17T09:58:00Z', 'fixture-secret')`)
	for _, deviceID := range []string{"device-a", "device-b"} {
		for _, stream := range []string{"organic", "inorganic", "b3"} {
			execFixture(t, db, `INSERT INTO slots (id, device_id, waste_stream, operational_capacity) VALUES (?, ?, ?, 10)`, deviceID+"-"+stream, deviceID, stream)
		}
	}

	raw := `{"device_id":"SIGAP-A","slots":[{"stream":"organic","fill":50,"weight":5,"gas_raw":42},{"stream":"inorganic","fill":30},{"stream":"b3","fill":10}]}`
	insertReading(t, db, "device-a-organic", "2026-09-17T09:59:40Z", raw, 50, 5, 50, "Normal", "Clear")
	insertReading(t, db, "device-a-inorganic", "2026-09-17T09:59:41Z", raw, 30, nil, nil, "Waspada", "Clear")
	insertReading(t, db, "device-a-b3", "2026-09-17T09:59:42Z", raw, 10, nil, nil, "Angkat", "Safety Alert")
	insertReading(t, db, "device-b-organic", "2026-09-17T09:57:00Z", raw, 0, nil, nil, "Normal", "Clear")

	return NewReadAPI(db, WithReadClock(func() time.Time { return time.Date(2026, 9, 17, 10, 0, 0, 0, time.UTC) }), WithOfflineAfter(30*time.Second)), db
}

func mixedOverviewFixture(t *testing.T) (*ReadAPI, *sql.DB) {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	execFixture(t, db, `INSERT INTO locations (id, name) VALUES ('location-mixed', 'Mixed')`)
	execFixture(t, db, `INSERT INTO devices (id, location_id, device_code, credential_digest) VALUES
		('device-safety', 'location-mixed', 'SIGAP-SAFETY', 'digest'),
		('device-pickup', 'location-mixed', 'SIGAP-PICKUP', 'digest')`)
	for _, deviceID := range []string{"device-safety", "device-pickup"} {
		for _, stream := range []string{"organic", "inorganic", "b3"} {
			execFixture(t, db, `INSERT INTO slots (id, device_id, waste_stream, operational_capacity) VALUES (?, ?, ?, 10)`, deviceID+"-"+stream, deviceID, stream)
		}
	}
	for _, stream := range []string{"organic", "inorganic"} {
		insertReading(t, db, "device-safety-"+stream, "2026-09-17T10:00:00Z", `{}`, 10, nil, nil, "Normal", "Clear")
	}
	insertReading(t, db, "device-safety-b3", "2026-09-17T10:00:00Z", `{}`, 10, nil, nil, "Normal", "Safety Alert")
	insertReading(t, db, "device-pickup-organic", "2026-09-17T10:00:00Z", `{}`, 10, nil, nil, "Angkat", "Clear")
	for _, stream := range []string{"inorganic", "b3"} {
		insertReading(t, db, "device-pickup-"+stream, "2026-09-17T10:00:00Z", `{}`, 10, nil, nil, "Normal", "Clear")
	}
	return NewReadAPI(db), db
}

func insertReading(t *testing.T, db *sql.DB, slotID, recordedAt, raw string, fill any, weight any, utilization any, fisStatus, safetyState string) {
	t.Helper()
	result, err := db.Exec(`INSERT INTO readings (slot_id, recorded_at, raw_payload, fill, weight, weight_utilization, sensor_health) VALUES (?, ?, ?, ?, ?, ?, '{"battery":90}')`, slotID, recordedAt, raw, fill, weight, utilization)
	if err != nil {
		t.Fatal(err)
	}
	readingID, err := result.LastInsertId()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO decisions (reading_id, fis_score, fis_status, fis_version, dominant_reason, safety_state) VALUES (?, 55, ?, 'fixture-v1', 'fixture reason', ?)`, readingID, fisStatus, safetyState); err != nil {
		t.Fatal(err)
	}
}

func execFixture(t *testing.T, db *sql.DB, statement string, args ...any) {
	t.Helper()
	if _, err := db.Exec(statement, args...); err != nil {
		t.Fatal(err)
	}
}

func decodeBody(t *testing.T, response *httptest.ResponseRecorder, destination any) {
	t.Helper()
	if err := json.Unmarshal(response.Body.Bytes(), destination); err != nil {
		t.Fatalf("decode response: %v body=%s", err, response.Body.String())
	}
}
