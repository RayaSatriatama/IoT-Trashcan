package httpapi

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

func TestSensorHistoryHTTPSelectsMonitoringBootAndExactPhysicalOrigin(t *testing.T) {
	f := newOriginFixture(t)
	// The metadata record owns sequence 1, so the sample needs its own sequence;
	// reusing it would produce the same record_id with a different hash.
	f.ingest(t, "SIGAP-01", "history-boot", originMetadata(t, "SIGAP-01", "boot-history", 1, "sigap-task6-physical4-v2"))
	f.ingest(t, "SIGAP-01", "history-sample", originSample(t, "SIGAP-01", "boot-history", "2", false))
	query, err := telemetryarchive.NewArchiveQuery(f.db)
	if err != nil {
		t.Fatal(err)
	}
	router := NewRouter(nil, WithReadDatabase(f.db), WithTelemetryArchive(telemetryarchive.NewService(f.db), query))
	r := httptest.NewRequest(http.MethodGet, "/api/devices/origin-id/sensor-history?sensor_id=mq4&limit=1", nil)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, r)
	if w.Code != http.StatusOK {
		t.Fatalf("history status %d: %s", w.Code, w.Body.String())
	}
	var got struct {
		DeviceID string `json:"device_id"`
		Points   []struct {
			Boot   string `json:"boot_id"`
			Origin string `json:"sample_origin"`
		} `json:"points"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got.DeviceID != "origin-id" || len(got.Points) != 1 || got.Points[0].Boot != "boot-history" || got.Points[0].Origin != "PHYSICAL" {
		t.Fatalf("history projection: %s", w.Body.String())
	}
}

func TestSensorHistoryHTTPRejectsNoncanonicalSensor(t *testing.T) {
	f := newOriginFixture(t)
	query, err := telemetryarchive.NewArchiveQuery(f.db)
	if err != nil {
		t.Fatal(err)
	}
	router := NewRouter(nil, WithReadDatabase(f.db), WithTelemetryArchive(telemetryarchive.NewService(f.db), query))
	r := httptest.NewRequest(http.MethodGet, "/api/devices/origin-id/sensor-history?sensor_id=%20mq4", nil)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("expected strict sensor ID rejection, got %d: %s", w.Code, w.Body.String())
	}
}
