package telemetryarchive

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestIngestRetainsEveryTelemetryRecordKind(t *testing.T) {
	db := openArchiveDB(t)
	service := NewService(db)
	for _, name := range []string{
		"event-clock-anchor.json",
		"metadata-profile-activation.json",
		"result-fuzzy-unavailable.json",
	} {
		body, err := os.ReadFile(filepath.Join("..", "..", "contracts", "telemetry", "v3", "fixtures", "valid", name))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := service.Ingest(context.Background(), archiveToken, body); err != nil {
			t.Fatalf("ingest %s: %v", name, err)
		}
	}
	rows, err := db.Query(`SELECT kind, record_bytes FROM telemetry_records ORDER BY kind`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	seen := map[string]bool{}
	for rows.Next() {
		var kind string
		var raw []byte
		if err := rows.Scan(&kind, &raw); err != nil {
			t.Fatal(err)
		}
		var object map[string]json.RawMessage
		if err := json.Unmarshal(raw, &object); err != nil {
			t.Fatalf("stored %s record is not JSON: %v", kind, err)
		}
		seen[kind] = true
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	for _, kind := range []string{"event", "metadata", "result"} {
		if !seen[kind] {
			t.Fatalf("missing archived record kind %q: %v", kind, seen)
		}
	}
}
