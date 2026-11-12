package devicedebug

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func TestHistoryPageJSONPreservesReceiptProvenanceAndCursors(t *testing.T) {
	requestID := uint32(0)
	relatedSequence := "0"
	page := HistoryPage{
		DeviceID:        "TEST-DEVICE",
		NextAfter:       "9223372036854775807",
		OldestAvailable: "9223372036854775806",
		RetentionGap:    true,
		HasMore:         true,
		Events: []HistoryEvent{
			{
				DebugEvent: DebugEvent{
					Sequence: "18446744073709551615", MonotonicUS: "9007199254740993",
					Level: "DEBUG", Module: 1, EventCode: 1,
					RequestID: &requestID, RelatedSequence: &relatedSequence,
				},
				IngestionID: "9223372036854775807", ReceivedAt: "2026-10-21T08:09:10Z",
				BootID: "0000000000000000000000000000000a", BuildID: "build-a", SnapshotLevel: "INFO",
				CapturedMonotonicUS: "9007199254740995", OldestSequence: "18446744073709551614",
				Overwritten: "18446744073709551615", DeliveryLost: "9007199254740997",
			},
			{
				DebugEvent:  DebugEvent{Sequence: "1", MonotonicUS: "3", Level: "INFO", Module: 1, EventCode: 1},
				IngestionID: "9223372036854775806", ReceivedAt: "2026-10-22T08:09:10Z",
				BootID: "0000000000000000000000000000000b", BuildID: "build-b", SnapshotLevel: "WARN",
				CapturedMonotonicUS: "4", OldestSequence: "1", Overwritten: "0", DeliveryLost: "0",
			},
		},
	}

	encoded, err := json.Marshal(page)
	if err != nil {
		t.Fatal(err)
	}
	var got map[string]json.RawMessage
	if err := json.Unmarshal(encoded, &got); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"device_id", "events", "next_after", "oldest_available", "retention_gap", "has_more"} {
		if _, ok := got[key]; !ok {
			t.Errorf("history page missing %q: %s", key, encoded)
		}
	}
	if _, obsolete := got["gap"]; obsolete {
		t.Fatalf("obsolete gap field emitted: %s", encoded)
	}
	var eventRows []map[string]json.RawMessage
	if err := json.Unmarshal(got["events"], &eventRows); err != nil {
		t.Fatal(err)
	}
	if len(eventRows) != 2 {
		t.Fatalf("got %d events, want 2", len(eventRows))
	}
	if string(got["next_after"]) != `"9223372036854775807"` || string(got["oldest_available"]) != `"9223372036854775806"` {
		t.Fatalf("server cursors lost string precision: %s", encoded)
	}
	for _, key := range []string{"sequence", "monotonic_us", "level", "module", "event_code", "request_id", "duration_us", "arg0", "arg1", "related_sequence", "ingestion_id", "received_at", "boot_id", "build_id", "snapshot_level", "captured_monotonic_us", "oldest_sequence", "overwritten", "delivery_lost"} {
		if _, ok := eventRows[0][key]; !ok {
			t.Errorf("history event missing %q: %s", key, encoded)
		}
	}
	if string(eventRows[0]["sequence"]) != `"18446744073709551615"` || string(eventRows[0]["monotonic_us"]) != `"9007199254740993"` {
		t.Fatalf("source uint64 strings changed: %s", encoded)
	}
	if string(eventRows[0]["next_after"]) != "" { // Cursor belongs to the page, not an event.
		t.Fatal("page cursor unexpectedly appeared on event")
	}
	if string(eventRows[0]["ingestion_id"]) != `"9223372036854775807"` || string(eventRows[1]["ingestion_id"]) != `"9223372036854775806"` {
		t.Fatalf("per-event ingestion IDs lost string precision: %s", encoded)
	}
	if string(eventRows[0]["request_id"]) != "0" || string(eventRows[0]["related_sequence"]) != `"0"` {
		t.Fatalf("optional zero changed: %s", encoded)
	}
	if string(eventRows[1]["request_id"]) != "null" || string(eventRows[1]["related_sequence"]) != "null" {
		t.Fatalf("optional null changed: %s", encoded)
	}
	if string(eventRows[0]["received_at"]) != `"2026-10-21T08:09:10Z"` || string(eventRows[0]["monotonic_us"]) == string(eventRows[0]["received_at"]) {
		t.Fatalf("server receipt and MCU monotonic time conflated: %s", encoded)
	}
	if string(eventRows[0]["level"]) != `"DEBUG"` || string(eventRows[0]["snapshot_level"]) != `"INFO"` {
		t.Fatalf("event and effective snapshot levels conflated: %s", encoded)
	}
	var roundTrip HistoryPage
	if err := json.Unmarshal(encoded, &roundTrip); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(page, roundTrip) {
		t.Fatalf("JSON roundtrip changed history page:\n got %#v\nwant %#v", roundTrip, page)
	}
}

func TestHistoryPageJSONEmitsExplicitEmptyEvents(t *testing.T) {
	page := HistoryPage{DeviceID: "TEST-DEVICE", Events: []HistoryEvent{}, NextAfter: "0", OldestAvailable: "0"}
	encoded, err := json.Marshal(page)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(encoded), `"events":[]`) {
		t.Fatalf("empty event page must encode events as [], got %s", encoded)
	}
}
