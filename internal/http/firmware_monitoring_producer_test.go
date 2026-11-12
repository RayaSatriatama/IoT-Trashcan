package httpapi

import (
	"database/sql"
	"encoding/json"
	"os"
	"reflect"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

// Optional second-stage check over the disposable DB produced by the existing
// actual-C++/Go ACK harness. Never point this at an operational database.
func TestMonitoringActualFirmwareArchive(t *testing.T) {
	path := os.Getenv("SIGAP_MONITORING_REPLAY_DB")
	if path == "" {
		t.Skip("requires retained actual-C++ host test DB")
	}
	db, err := sql.Open("sqlite", "file:"+path+"?mode=ro")
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	f := &originFixture{db: db, router: NewRouter(nil, WithReadDatabase(db))}
	var owner string
	if err := db.QueryRow("SELECT device_code FROM devices WHERE id='firmware-host-test'").Scan(&owner); err != nil {
		t.Fatal(err)
	}
	snapshot, err := telemetryarchive.Current(t.Context(), db, owner)
	if err != nil {
		t.Fatal(err)
	}
	actual := monitoringRead(t, f, "/api/devices/firmware-host-test", "firmware-host-test")
	if len(actual) != 13 {
		t.Fatalf("actual producer monitoring sensor count %d", len(actual))
	}
	for _, item := range actual {
		var view firmwareMonitoringDTO
		encoded, err := json.Marshal(item)
		if err != nil {
			t.Fatal(err)
		}
		if err := json.Unmarshal(encoded, &view); err != nil {
			t.Fatal(err)
		}
		var source struct {
			SensorID  string          `json:"sensor_id"`
			BootID    string          `json:"boot_id"`
			Sequence  string          `json:"sequence"`
			Quality   string          `json:"quality"`
			Reason    string          `json:"reason"`
			Raw       json.RawMessage `json:"raw"`
			Converted json.RawMessage `json:"converted"`
		}
		if err := json.Unmarshal(snapshot.Samples[view.Stream][view.SensorID], &source); err != nil {
			t.Fatal(err)
		}
		if view.SensorID != source.SensorID || view.BootID != source.BootID || view.Sequence != source.Sequence || view.Quality != source.Quality || view.Reason != source.Reason || view.SampleOrigin != "SIMULATED" {
			t.Fatalf("actual producer identity/quality/origin mismatch for %s", view.SensorID)
		}
		if !reflect.DeepEqual(view.Raw, monitoringNumbers(source.Raw, monitoringRawKeys)) {
			t.Fatalf("actual producer numeric raw mismatch for %s", view.SensorID)
		}
		var converted map[string]float64
		if source.Quality == "READY" {
			converted = monitoringNumbers(source.Converted, monitoringConvertedKeys)
		}
		if !reflect.DeepEqual(view.Converted, converted) {
			t.Fatalf("actual producer conversion mismatch for %s", view.SensorID)
		}
	}
}
