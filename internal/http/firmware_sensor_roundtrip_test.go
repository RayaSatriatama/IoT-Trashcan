package httpapi

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/devicecontrol"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetry"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/webapp"
)

// Invoked by the Python host compiler harness, never an installed-device test.
func dallasCRC(data []byte) byte {
	var crc byte
	for _, value := range data {
		crc ^= value
		for bit := 0; bit < 8; bit++ {
			if crc&1 != 0 {
				crc = crc>>1 ^ 0x8c
			} else {
				crc >>= 1
			}
		}
	}
	return crc
}

func TestFirmwareSensorRoundtrip(t *testing.T) {
	producer := os.Getenv("SIGAP_SENSOR_TEST_PRODUCER")
	if producer == "" {
		t.Skip("run tests/test_firmware_sensor_roundtrip.py for actual C++ producer coverage")
	}
	directory := os.Getenv("SIGAP_SENSOR_TEST_ARTIFACTS")
	if directory == "" {
		directory = t.TempDir()
	}
	if !filepath.IsAbs(directory) {
		t.Fatal("artifact directory must be absolute and fresh")
	}
	entries, err := os.ReadDir(directory)
	if err != nil || len(entries) != 0 {
		t.Fatalf("require empty disposable directory: %v", err)
	}
	path := filepath.Join(directory, "host.sqlite")
	created, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	_ = created.Close()
	db, err := database.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES('firmware-host-location','Firmware HOST TEST')`); err != nil {
		t.Fatal(err)
	}
	control := devicecontrol.NewStore(db)
	auth := devicecontrol.NewDatabaseAuthorizer(db)
	query, err := telemetryarchive.NewArchiveQuery(db)
	if err != nil {
		t.Fatal(err)
	}
	router := NewRouter(ingest.NewService(db), WithReadDatabase(db), WithSimulationEnabled(false), WithTelemetryArchive(telemetryarchive.NewService(db), query), WithDeviceControl(control, devicecontrol.HTTPOptions{Operator: auth, Device: auth, Diagnostics: control}))
	server := httptest.NewUnstartedServer(webapp.Handler(router))
	browser := os.Getenv("SIGAP_SENSOR_TEST_BROWSER") == "1"
	if browser {
		listener, err := net.Listen("tcp", "127.0.0.1:18772")
		if err != nil {
			t.Fatal(err)
		}
		_ = server.Listener.Close()
		server.Listener = listener
	}
	server.Start()
	defer server.Close()
	client := server.Client()
	client.Timeout = 5 * time.Second
	do := func(method, path, token string, body []byte) (int, []byte) {
		t.Helper()
		req, err := http.NewRequest(method, server.URL+path, bytes.NewReader(body))
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("Authorization", "Bearer "+token)
		req.Header.Set("Content-Type", "application/json")
		response, err := client.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer response.Body.Close()
		var data bytes.Buffer
		if _, err := data.ReadFrom(response.Body); err != nil {
			t.Fatal(err)
		}
		return response.StatusCode, data.Bytes()
	}
	ctx, cancel := context.WithTimeout(t.Context(), 90*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, producer)
	output, err := command.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	input, err := command.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	command.Stderr = &stderr
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = command.Process.Kill(); _ = input.Close() }()
	scanner := bufio.NewScanner(output)
	scanner.Buffer(make([]byte, 32768), 65536)
	latest := map[string]map[string]json.RawMessage{}
	device, boot := "", ""
	batches, results, boots, done := 0, 0, 0, false
	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, "DONE ") {
			count, err := strconv.Atoi(strings.TrimPrefix(line, "DONE "))
			if err != nil || count != batches {
				t.Fatalf("C++ delivery count mismatch: %q vs %d", line, batches)
			}
			done = true
			continue
		}
		if !strings.HasPrefix(line, "BATCH ") {
			t.Fatalf("unexpected producer output %q", line)
		}
		body := []byte(strings.TrimPrefix(line, "BATCH "))
		var envelope struct {
			Device   string                       `json:"device_id"`
			Batch    string                       `json:"batch_id"`
			Samples  []map[string]json.RawMessage `json:"samples"`
			Results  []json.RawMessage            `json:"results"`
			Metadata []struct {
				Type    string `json:"metadata_type"`
				Boot    string `json:"boot_id"`
				Payload struct {
					Version string `json:"firmware_version"`
				} `json:"payload"`
			} `json:"metadata"`
		}
		if err := json.Unmarshal(body, &envelope); err != nil {
			t.Fatal(err)
		}
		if device == "" {
			device = envelope.Device
			sum := sha256.Sum256([]byte("TEST-DEVICE-TOKEN"))
			if _, err := db.Exec(`INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES('firmware-host-test','firmware-host-location',?,?)`, device, hex.EncodeToString(sum[:])); err != nil {
				t.Fatal(err)
			}
			if err := devicecontrol.ProvisionOperatorSession(t.Context(), db, "sensor-host-session", "sensor-host-operator", "sensor-host-operator-token", "sensor-host-csrf", time.Now().Add(time.Hour), []string{device}); err != nil {
				t.Fatal(err)
			}
		}
		if device != envelope.Device {
			t.Fatal("producer device changed")
		}
		// Keep rejected producer bytes too; a failing HTTP response must not
		// erase the only reproducible cross-language evidence.
		if err := os.WriteFile(filepath.Join(directory, fmt.Sprintf("%04d.attempt.batch.json", batches)), body, 0600); err != nil {
			t.Fatal(err)
		}
		status, ack := do(http.MethodPost, "/api/telemetry/batches", "TEST-DEVICE-TOKEN", body)
		if status != http.StatusOK {
			_, validationErr := telemetry.ValidateBatch(body)
			t.Fatalf("actual producer ingest %d: %s; contract=%v; retained=%s; stderr=%s", status, ack, validationErr, directory, stderr.String())
		}
		if _, err := telemetry.ValidateAck(ack, telemetry.AckExpectation{DeviceID: device, BatchID: envelope.Batch, BodyHash: telemetry.BodyHash(body), RecordCount: 1}); err != nil {
			t.Fatal(err)
		}
		var durable []byte
		if err := db.QueryRow(`SELECT body_bytes FROM telemetry_batches WHERE device_id=? AND batch_id=?`, device, envelope.Batch).Scan(&durable); err != nil || !bytes.Equal(durable, body) {
			t.Fatalf("durable exact-byte mismatch: %v", err)
		}
		for suffix, data := range map[string][]byte{"batch": body, "ack": ack} {
			if err := os.WriteFile(filepath.Join(directory, fmt.Sprintf("%04d.%s.json", batches, suffix)), data, 0600); err != nil {
				t.Fatal(err)
			}
		}
		for _, sample := range envelope.Samples {
			var sensor string
			if err := json.Unmarshal(sample["sensor_id"], &sensor); err != nil {
				t.Fatal(err)
			}
			latest[sensor] = sample
		}
		results += len(envelope.Results)
		for _, metadata := range envelope.Metadata {
			if metadata.Type == "BOOT" {
				boots++
				boot = metadata.Boot
				if metadata.Payload.Version != "sigap-task6-sensor-sim-v1" {
					t.Fatal("wrong actual app BOOT mode")
				}
			}
		}
		// The real HTTP JSON writer already includes a final LF. Preserve one
		// line delimiter rather than feeding a blank ACK to the next request.
		if _, err := input.Write(ack); err != nil {
			t.Fatal(err)
		}
		if len(ack) == 0 || ack[len(ack)-1] != '\n' {
			if _, err := input.Write([]byte{'\n'}); err != nil {
				t.Fatal(err)
			}
		}
		batches++
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if err := command.Wait(); err != nil {
		t.Fatalf("producer failed: %v stderr=%s", err, stderr.String())
	}
	if !done || boots != 1 || results == 0 || len(latest) != 13 {
		t.Fatalf("incomplete graph: done=%t boot=%d results=%d sensor=%d", done, boots, results, len(latest))
	}
	for _, sensor := range []string{"hx711_organic", "hx711_inorganic", "hx711_b3", "ultrasonic_organic", "ultrasonic_inorganic", "ultrasonic_b3", "mq4", "mq135", "mq7", "mq8", "ds18b20_b3", "sht31_44", "sht31_45"} {
		sample := latest[sensor]
		var raw map[string]json.RawMessage
		if err := json.Unmarshal(sample["raw"], &raw); err != nil || len(raw) == 0 {
			t.Fatalf("%s lacks actual producer raw evidence: %s", sensor, sample["raw"])
		}
		switch sensor {
		case "mq7":
			var phase string
			if err := json.Unmarshal(raw["heater_phase"], &phase); err != nil ||
				!(phase == "PREHEAT" || phase == "HIGH_CLEAN" || phase == "LOW_DETECT" || phase == "FAULT") {
				t.Fatalf("MQ-7 must preserve a real controller phase, got %s", sample["raw"])
			}
		case "ds18b20_b3":
			var evidence struct {
				ROM        string `json:"rom"`
				Scratch    string `json:"scratchpad_hex"`
				Resolution uint8  `json:"resolution_bits"`
				CRC        bool   `json:"crc_valid"`
			}
			if err := json.Unmarshal(sample["raw"], &evidence); err != nil || len(evidence.ROM) != 16 || len(evidence.Scratch) != 18 || !evidence.CRC || evidence.Resolution < 9 || evidence.Resolution > 12 {
				t.Fatalf("DS18B20 generated protocol bytes missing/invalid: %s", sample["raw"])
			}
			rom, romErr := hex.DecodeString(evidence.ROM)
			scratch, scratchErr := hex.DecodeString(evidence.Scratch)
			if romErr != nil || scratchErr != nil || dallasCRC(rom[:7]) != rom[7] || dallasCRC(scratch[:8]) != scratch[8] {
				t.Fatalf("DS18B20 ROM/scratchpad CRC mismatch: %s", sample["raw"])
			}
		case "sht31_44", "sht31_45":
			var evidence struct {
				Temperature uint16 `json:"temperature_ticks"`
				Humidity    uint16 `json:"humidity_ticks"`
				TempCRC     bool   `json:"temperature_crc_valid"`
				HumCRC      bool   `json:"humidity_crc_valid"`
			}
			if err := json.Unmarshal(sample["raw"], &evidence); err != nil || !evidence.TempCRC || !evidence.HumCRC {
				t.Fatalf("%s original uint16 ticks/CRCs missing: %s", sensor, sample["raw"])
			}
		default:
			field := map[string]string{"hx711_organic": "counts", "hx711_inorganic": "counts", "hx711_b3": "counts", "ultrasonic_organic": "echo_us", "ultrasonic_inorganic": "echo_us", "ultrasonic_b3": "echo_us", "mq4": "adc_counts", "mq135": "adc_counts", "mq8": "adc_counts"}[sensor]
			if field == "" || len(raw[field]) == 0 || string(raw[field]) == "null" {
				t.Fatalf("%s missing required raw field %s: %s", sensor, field, sample["raw"])
			}
		}
		status, body := do(http.MethodGet, "/api/device-control/commands/"+device+"/diagnostics?sensor_id="+sensor, "sensor-host-operator-token", nil)
		if status != http.StatusOK {
			t.Fatalf("diagnostic %s status=%d: %s", sensor, status, body)
		}
		var d map[string]json.RawMessage
		if err := json.Unmarshal(body, &d); err != nil {
			t.Fatal(err)
		}
		if string(d["sample_origin"]) != `"SIMULATED"` || string(d["boot_id"]) != strconv.Quote(boot) {
			t.Fatalf("origin/boot mismatch %s", body)
		}
		for src, dst := range map[string]string{"raw": "raw", "converted": "converted_value", "sequence": "last_sequence", "quality": "quality"} {
			if !bytes.Equal(latest[sensor][src], d[dst]) {
				t.Fatalf("%s %s changed: actual=%s source=%s", sensor, src, d[dst], latest[sensor][src])
			}
		}
		if err := os.WriteFile(filepath.Join(directory, sensor+".diagnostic.json"), body, 0600); err != nil {
			t.Fatal(err)
		}
	}
	ready, _ := json.Marshal(map[string]any{"origin": server.URL, "deviceId": "firmware-host-test", "deviceCode": device, "batches": batches, "results": results, "sensors": len(latest), "goSimulator": false, "physical": false})
	if err := os.WriteFile(filepath.Join(directory, "ready.json"), ready, 0600); err != nil {
		t.Fatal(err)
	}
	t.Logf("HOST roundtrip: batches=%d results=%d sensors=%d; actual C++ ACK/context/journal/reclaim accepted", batches, results, len(latest))
	if browser {
		deadline := time.NewTimer(150 * time.Second)
		defer deadline.Stop()
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-deadline.C:
				t.Fatal("browser hold expired without explicit stop")
			case <-ticker.C:
				if _, err := os.Stat(filepath.Join(directory, "stop")); err == nil {
					return
				}
			}
		}
	}
}
