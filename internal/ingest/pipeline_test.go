package ingest

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"math"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/fuzzy"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/safety"

	_ "modernc.org/sqlite"
)

func TestNormalizeReadingPreservesMissingValuesAndComputesWeightUtilization(t *testing.T) {
	fill, weight := 42.0, 15.0
	capacity := 30.0

	normalized, err := Normalize(Slot{Fill: &fill, Weight: &weight}, &capacity)
	if err != nil {
		t.Fatal(err)
	}
	if normalized.FillPct == nil || *normalized.FillPct != 42 {
		t.Fatalf("FillPct = %#v, want 42", normalized.FillPct)
	}
	if normalized.WeightUtilization == nil || *normalized.WeightUtilization != 50 {
		t.Fatalf("WeightUtilization = %#v, want 50", normalized.WeightUtilization)
	}
	if normalized.OGI != nil || normalized.AccumulationHours != nil {
		t.Fatalf("unresolved normalized values must be nil: %#v", normalized)
	}

	missing, err := Normalize(Slot{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if missing.FillPct != nil || missing.WeightUtilization != nil {
		t.Fatalf("missing values must remain nil: %#v", missing)
	}
}

func TestNormalizeReadingRejectsNonPositiveOperationalCapacity(t *testing.T) {
	weight, capacity := 1.0, 0.0
	_, err := Normalize(Slot{Weight: &weight}, &capacity)
	if !errors.Is(err, ErrInvalidCapacity) {
		t.Fatalf("Normalize() error = %v, want ErrInvalidCapacity", err)
	}
}

func TestIngestExactReplayDoesNotDuplicateReadingsOrDecisions(t *testing.T) {
	db, service := decisionTestService(t, testDecisionEngine(t))
	defer db.Close()
	req := decisionRequest()
	raw := []byte(`{"device_id":"SIGAP-01","recorded_at":"2026-09-16T18:00:00+07:00","slots":[{"stream":"organic","fill":72},{"stream":"inorganic","weight":0},{"stream":"b3"}]}`)

	first, err := service.Ingest(context.Background(), "fixture-token", req, raw)
	if err != nil {
		t.Fatal(err)
	}
	if first.ReadingsStored != 3 || first.Deduplicated {
		t.Fatalf("first result = %#v", first)
	}
	second, err := service.Ingest(context.Background(), "fixture-token", req, raw)
	if err != nil {
		t.Fatal(err)
	}
	if !second.Deduplicated || second.ReadingsStored != 0 {
		t.Fatalf("second result = %#v, want deduplicated replay", second)
	}
	assertCount(t, db, "readings", 3)
	assertCount(t, db, "decisions", 3)
}

func TestIngestPersistsConfiguredDecisionAndSafetyOverride(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	co := 0.9
	req := decisionRequest()
	req.Slots[2].Sensors = []SensorReading{{SensorID: "mq7-b3", SensorType: "mq7", Parameter: "co", NormalizedValue: &co, Unit: "index", SensorState: SensorValid}}
	service.safetyConfig = safety.Config{Thresholds: []safety.Threshold{{
		Signal: safety.SignalMQ7CO, AtOrAbove: 0.8, Reason: "test-only CO fixture",
	}}}

	if _, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"decision":true}`)); err != nil {
		t.Fatal(err)
	}

	var score float64
	var fisStatus, fisVersion, reason, safetyState string
	if err := db.QueryRow(`
		SELECT fis_score, fis_status, fis_version, dominant_reason, safety_state
		FROM decisions WHERE reading_id = 3`).Scan(&score, &fisStatus, &fisVersion, &reason, &safetyState); err != nil {
		t.Fatal(err)
	}
	if score != 10 || fisStatus != "Normal" || fisVersion != "test-fis-v1" || reason != "fixture" || safetyState != safety.StateSafetyAlert {
		t.Fatalf("decision = score=%v status=%q version=%q reason=%q safety=%q", score, fisStatus, fisVersion, reason, safetyState)
	}
}

func TestIngestPersistsEngineVersionWhenResultOmitsIt(t *testing.T) {
	db, service := decisionTestService(t, &versionlessEngine{})
	defer db.Close()

	if _, err := service.Ingest(context.Background(), "fixture-token", decisionRequest(), []byte(`{"version":true}`)); err != nil {
		t.Fatal(err)
	}
	var version string
	if err := db.QueryRow(`SELECT fis_version FROM decisions ORDER BY reading_id LIMIT 1`).Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != "engine-version" {
		t.Fatalf("fis_version = %q, want engine version", version)
	}
}

func TestIngestSelectsDecisionProfileByStream(t *testing.T) {
	db, service := decisionTestService(t, nil)
	defer db.Close()
	service.engines = map[string]fuzzy.Engine{
		"organic":   fixedEngine{version: "organic-profile"},
		"inorganic": fixedEngine{version: "inorganic-profile"},
		"b3":        fixedEngine{version: "b3-profile"},
	}
	if _, err := service.Ingest(context.Background(), "fixture-token", decisionRequest(), []byte(`{"profiles":true}`)); err != nil {
		t.Fatal(err)
	}
	rows, err := db.Query(`SELECT s.waste_stream, d.fis_version FROM decisions d JOIN readings r ON r.id=d.reading_id JOIN slots s ON s.id=r.slot_id ORDER BY d.reading_id`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	want := map[string]string{"organic": "organic-profile", "inorganic": "inorganic-profile", "b3": "b3-profile"}
	for rows.Next() {
		var stream, version string
		if err := rows.Scan(&stream, &version); err != nil {
			t.Fatal(err)
		}
		if version != want[stream] {
			t.Fatalf("%s version=%q want=%q", stream, version, want[stream])
		}
	}
}

func TestIngestPersistsDataInvalidForNonValidCollectionSensor(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	req := decisionRequest()
	req.Slots[0].Sensors = []SensorReading{{SensorID: "organic-mq135", SensorType: "mq135", Parameter: "broad_gas", SensorState: SensorWarmingUp}}
	if _, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"warming":true}`)); err != nil {
		t.Fatal(err)
	}
	var status, reason string
	if err := db.QueryRow(`SELECT fis_status, dominant_reason FROM decisions ORDER BY reading_id LIMIT 1`).Scan(&status, &reason); err != nil {
		t.Fatal(err)
	}
	if status != "Data Invalid" || !strings.Contains(reason, "WARMING_UP") {
		t.Fatalf("status=%q reason=%q", status, reason)
	}
}

func TestIngestPersistsDataInvalidWhenFuzzyInputIsMissing(t *testing.T) {
	db, service := decisionTestService(t, missingInputEngine{})
	defer db.Close()

	if _, err := service.Ingest(context.Background(), "fixture-token", decisionRequest(), []byte(`{"missing-input":true}`)); err != nil {
		t.Fatal(err)
	}
	var decisions, invalid int
	if err := db.QueryRow(`SELECT COUNT(*), COUNT(CASE WHEN fis_status = 'Data Invalid' THEN 1 END) FROM decisions`).Scan(&decisions, &invalid); err != nil {
		t.Fatal(err)
	}
	if decisions != 3 || invalid != 3 {
		t.Fatalf("decisions=%d invalid=%d, want three auditable Data Invalid decisions", decisions, invalid)
	}
}

func TestIngestRejectsSameTimestampWithDifferentRawPayloadWithoutWrites(t *testing.T) {
	db, service := decisionTestService(t, testDecisionEngine(t))
	defer db.Close()
	req := decisionRequest()
	if _, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"payload":"first"}`)); err != nil {
		t.Fatal(err)
	}
	_, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"payload":"changed"}`))
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("Ingest() error = %v, want ErrConflict", err)
	}
	assertCount(t, db, "readings", 3)
	assertCount(t, db, "decisions", 3)
}

func TestIngestPersistsDataInvalidDecisionWithoutCallingEngine(t *testing.T) {
	db, service := decisionTestService(t, &recordingEngine{})
	defer db.Close()
	req := decisionRequest()
	invalid := 101.0
	for index := range req.Slots {
		req.Slots[index].Fill = &invalid
	}

	if _, err := service.Ingest(context.Background(), "fixture-token", req, []byte(`{"invalid":true}`)); err != nil {
		t.Fatal(err)
	}
	if service.engine.(*recordingEngine).calls != 0 {
		t.Fatal("FIS engine was called for invalid data")
	}
	var score sql.NullFloat64
	var status, reason string
	if err := db.QueryRow(`SELECT fis_score, fis_status, dominant_reason FROM decisions ORDER BY reading_id LIMIT 1`).Scan(&score, &status, &reason); err != nil {
		t.Fatal(err)
	}
	if score.Valid || status != "Data Invalid" || !strings.Contains(reason, "fill") {
		t.Fatalf("invalid decision score=%#v status=%q reason=%q", score, status, reason)
	}
	assertCount(t, db, "readings", 3)
	assertCount(t, db, "decisions", 3)
}

func TestIngestAcceptsMeasuredZeroAndKeepsMissingDistinct(t *testing.T) {
	zero := 0.0
	if result := ValidateOperational(Slot{Fill: &zero}); !result.Valid {
		t.Fatalf("zero fill was invalid: %#v", result)
	}
	if result := ValidateOperational(Slot{}); !result.Valid {
		t.Fatalf("missing fill was invalid: %#v", result)
	}
	negative := -1.0
	if result := ValidateOperational(Slot{Weight: &negative}); result.Valid {
		t.Fatal("negative weight was accepted")
	}
}

func TestValidateOperationalAcceptsDocumentedSensorStatesAndLegacySlots(t *testing.T) {
	if result := ValidateOperational(Slot{}); !result.Valid {
		t.Fatalf("legacy slot was rejected: %#v", result)
	}
	for _, state := range []SensorState{SensorValid, SensorWarmingUp, SensorStale, SensorInvalid, SensorDisabled} {
		slot := Slot{Sensors: []SensorReading{{
			SensorID: "sensor-1", SensorType: "mq135", Parameter: "broad_gas", SensorState: state,
		}}}
		if result := ValidateOperational(slot); !result.Valid {
			t.Fatalf("state %q was rejected: %#v", state, result)
		}
	}
}

func TestValidateOperationalRejectsUnknownSensorState(t *testing.T) {
	result := ValidateOperational(Slot{Sensors: []SensorReading{{
		SensorID: "sensor-1", SensorType: "mq135", Parameter: "broad_gas", SensorState: SensorState("UNKNOWN"),
	}}})
	if result.Valid || !strings.Contains(strings.Join(result.Reasons, "; "), "sensor_state") {
		t.Fatalf("ValidateOperational() = %#v, want sensor_state error", result)
	}
}

func TestValidateOperationalRequiresCalibrationMetadataForPPM(t *testing.T) {
	value := 2.5
	normalized := 0.25
	tests := []struct {
		name   string
		sensor SensorReading
		valid  bool
	}{
		{name: "calibrated ppm", sensor: SensorReading{SensorID: "mq7", SensorType: "mq7", Parameter: "co", Unit: "ppm", EngineeringValue: &value, CalibrationVersion: "mq7-lab-v1", SensorState: SensorValid}, valid: true},
		{name: "ppm missing engineering value", sensor: SensorReading{SensorID: "mq7", SensorType: "mq7", Parameter: "co", Unit: "ppm", CalibrationVersion: "mq7-lab-v1", SensorState: SensorValid}},
		{name: "ppm missing calibration", sensor: SensorReading{SensorID: "mq7", SensorType: "mq7", Parameter: "co", Unit: "ppm", EngineeringValue: &value, SensorState: SensorValid}},
		{name: "normalized index needs no calibration", sensor: SensorReading{SensorID: "mq7", SensorType: "mq7", Parameter: "co", Unit: "index", NormalizedValue: &normalized, SensorState: SensorValid}, valid: true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			result := ValidateOperational(Slot{Sensors: []SensorReading{tt.sensor}})
			if result.Valid != tt.valid {
				t.Fatalf("ValidateOperational() = %#v, want valid=%v", result, tt.valid)
			}
		})
	}
}

func TestValidateOperationalRejectsNonFiniteAndOutOfRangeSensorEvidence(t *testing.T) {
	nan := math.NaN()
	outOfRange := 1.1
	result := ValidateOperational(Slot{Sensors: []SensorReading{
		{SensorID: "mq7", SensorType: "mq7", Parameter: "co", RawValue: &nan, SensorState: SensorValid},
		{SensorID: "mq8", SensorType: "mq8", Parameter: "h2", NormalizedValue: &outOfRange, SensorState: SensorValid},
	}})
	joined := strings.Join(result.Reasons, "; ")
	if result.Valid || !strings.Contains(joined, "raw_value") || !strings.Contains(joined, "normalized_value") {
		t.Fatalf("ValidateOperational() = %#v, want numeric sensor evidence errors", result)
	}
}

func TestIngestRollsBackReadingsDecisionsAndHeartbeatWhenDecisionInsertFails(t *testing.T) {
	db, service := decisionTestService(t, testDecisionEngine(t))
	defer db.Close()
	if _, err := db.Exec(`
		CREATE TRIGGER fail_third_decision
		BEFORE INSERT ON decisions
		WHEN NEW.reading_id = 3
		BEGIN
			SELECT RAISE(ABORT, 'forced decision failure');
		END`); err != nil {
		t.Fatal(err)
	}

	_, err := service.Ingest(context.Background(), "fixture-token", decisionRequest(), []byte(`{"rollback":true}`))
	if !errors.Is(err, ErrPersistence) || !strings.Contains(err.Error(), "forced decision failure") {
		t.Fatalf("Ingest() error = %v", err)
	}
	assertCount(t, db, "readings", 0)
	assertCount(t, db, "decisions", 0)
	var lastSeen string
	if err := db.QueryRow(`SELECT last_seen FROM devices WHERE id = 'device-1'`).Scan(&lastSeen); err != nil {
		t.Fatal(err)
	}
	if lastSeen != "before-ingest" {
		t.Fatalf("last_seen = %q, want before-ingest", lastSeen)
	}
}

type recordingEngine struct{ calls int }

func (e *recordingEngine) Evaluate(input fuzzy.Input) (fuzzy.Result, error) {
	e.calls++
	return fuzzy.Result{Score: 10, Status: "Normal", DominantReason: "fixture", Version: "test-fis-v1"}, nil
}

func (e *recordingEngine) Version() string { return "test-fis-v1" }

type versionlessEngine struct{}

func (e *versionlessEngine) Evaluate(fuzzy.Input) (fuzzy.Result, error) {
	return fuzzy.Result{Score: 20, Status: "Normal", DominantReason: "version fixture"}, nil
}

func (e *versionlessEngine) Version() string { return "engine-version" }

type fixedEngine struct{ version string }

func (e fixedEngine) Evaluate(input fuzzy.Input) (fuzzy.Result, error) {
	return fuzzy.Result{Score: 25, Status: "Normal", DominantReason: "profile fixture", Version: e.version}, nil
}

func (e fixedEngine) Version() string { return e.version }

type missingInputEngine struct{}

func (missingInputEngine) Evaluate(fuzzy.Input) (fuzzy.Result, error) {
	return fuzzy.Result{}, fmt.Errorf("%w: accumulation_hours", fuzzy.ErrMissingInput)
}

func (missingInputEngine) Version() string { return "four-input-v1" }

func testDecisionEngine(t *testing.T) fuzzy.Engine {
	t.Helper()
	engine, err := fuzzy.NewEngine(fuzzy.Config{
		Version: "test-fis-v1",
		Inputs: map[string]fuzzy.InputVariable{
			"fill": {Sets: map[string]fuzzy.Membership{"any": fuzzy.TrapezoidMembership(0, 0, 100, 100)}},
		},
		Outputs: map[string]fuzzy.OutputSet{
			"normal": {Status: "Normal", Membership: fuzzy.TriangleMembership(0, 0, 100)},
		},
		Rules:       []fuzzy.Rule{{ID: "test", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "any"}}, Output: "normal", Reason: "test fixture"}},
		OutputRange: fuzzy.Range{Min: 0, Max: 100, Step: 1},
	})
	if err != nil {
		t.Fatal(err)
	}
	return engine
}

func decisionRequest() Request {
	req := validRequest()
	for index, fill := range []float64{72, 40, 10} {
		req.Slots[index].Fill = &fill
	}
	return req
}

func decisionTestService(t *testing.T, engine fuzzy.Engine) (*sql.DB, *Service) {
	t.Helper()
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	schema := `
		CREATE TABLE devices (id TEXT PRIMARY KEY, device_code TEXT NOT NULL UNIQUE, credential_digest TEXT NOT NULL, last_seen TEXT);
		CREATE TABLE slots (id TEXT PRIMARY KEY, device_id TEXT NOT NULL, waste_stream TEXT NOT NULL, operational_capacity REAL, UNIQUE(device_id, waste_stream));
		CREATE TABLE readings (id INTEGER PRIMARY KEY, slot_id TEXT NOT NULL, recorded_at TEXT NOT NULL, raw_payload TEXT NOT NULL, fill REAL, weight REAL, weight_utilization REAL, ogi REAL, accumulation REAL, temperature REAL, sensor_health TEXT);
		CREATE TABLE decisions (reading_id INTEGER PRIMARY KEY, fis_score REAL, fis_status TEXT, fis_version TEXT, dominant_reason TEXT, safety_state TEXT, trace_json TEXT, safety_json TEXT);
	`
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		t.Fatal(err)
	}
	digest := sha256.Sum256([]byte("fixture-token"))
	if _, err := db.Exec(`INSERT INTO devices (id, device_code, credential_digest, last_seen) VALUES ('device-1', 'SIGAP-01', ?, 'before-ingest')`, hex.EncodeToString(digest[:])); err != nil {
		db.Close()
		t.Fatal(err)
	}
	for _, stream := range []string{"organic", "inorganic", "b3"} {
		if _, err := db.Exec(`INSERT INTO slots (id, device_id, waste_stream, operational_capacity) VALUES (?, 'device-1', ?, 20)`, "slot-"+stream, stream); err != nil {
			db.Close()
			t.Fatal(err)
		}
	}
	service := NewService(db, WithDecisionEngine(engine), WithSafetyConfig(safety.Config{}))
	service.now = func() time.Time { return time.Date(2026, 9, 17, 12, 0, 0, 0, time.UTC) }
	return db, service
}

func assertCount(t *testing.T, db *sql.DB, table string, want int) {
	t.Helper()
	var got int
	if err := db.QueryRow(`SELECT COUNT(*) FROM ` + table).Scan(&got); err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Fatalf("%s count = %d, want %d", table, got, want)
	}
}
