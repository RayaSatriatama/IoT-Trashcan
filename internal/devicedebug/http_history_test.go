package devicedebug

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"
)

func historyHandler(s *Store) http.Handler {
	return NewHandler(s, HTTPOptions{Device: testAuthorizer{}, Operator: testAuthorizer{operator: "operator"}})
}

func historyPost(t *testing.T, h http.Handler, u Upload) (int, UploadAck) {
	t.Helper()
	if _, err := DecodeUpload(bodyFor(t, u)); err != nil {
		t.Fatalf("invalid fixture: %v", err)
	}
	r := httptest.NewRequest(http.MethodPost, "/api/device-debug/device/"+u.DeviceID+"/events", strings.NewReader(string(bodyFor(t, u))))
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	var ack UploadAck
	if w.Code == http.StatusOK {
		if err := json.Unmarshal(w.Body.Bytes(), &ack); err != nil {
			t.Fatal(err)
		}
	}
	return w.Code, ack
}

func historyGet(t *testing.T, h http.Handler, path string) HistoryPage {
	t.Helper()
	w := httptest.NewRecorder()
	h.ServeHTTP(w, httptest.NewRequest(http.MethodGet, path, nil))
	if w.Code != http.StatusOK {
		t.Fatalf("GET %s status=%d body=%s", path, w.Code, w.Body)
	}
	var p HistoryPage
	if err := json.Unmarshal(w.Body.Bytes(), &p); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestHTTPHistoryNullZeroAndCompleteProvenance(t *testing.T) {
	s, _, _ := testDebugStore(t, DefaultPolicy())
	h := historyHandler(s)
	u := makeUpload("TEST-DEVICE", "00000000000000000000000000000032", "build-a", 2, 2)
	u.Level, u.CapturedMonotonicUS, u.OldestSequence, u.Overwritten, u.DeliveryLost = "DEBUG", "18446744073709551615", "1", "3", "4"
	u.Events[0].Level, u.Events[0].MonotonicUS, u.Events[0].Arg0, u.Events[0].Arg1 = "WARN", "10", 1, 2
	u.Events[0].DurationUS = 0
	u.Events[1].Sequence, u.Events[1].MonotonicUS = "18446744073709551615", "18446744073709551615"
	maxRelated := "18446744073709551615"
	u.Events[1].RelatedSequence = &maxRelated
	u.Events[1].Level, u.Events[1].Arg0, u.Events[1].Arg1 = "INFO", 2, 5
	if status, ack := historyPost(t, h, u); status != 200 || ack.Status != "DURABLE" || ack.StoredCount != 2 || ack.DeduplicatedCount != 0 {
		t.Fatalf("POST status=%d ack=%+v", status, ack)
	}
	page := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE")
	if len(page.Events) != 2 {
		t.Fatalf("events=%+v", page.Events)
	}
	for i, e := range page.Events {
		if e.BootID != u.BootID || e.BuildID != u.BuildID || e.SnapshotLevel != "DEBUG" || e.CapturedMonotonicUS != u.CapturedMonotonicUS || e.OldestSequence != "1" || e.Overwritten != "3" || e.DeliveryLost != "4" || e.ReceivedAt == "" || e.IngestionID == "" {
			t.Fatalf("provenance[%d]=%+v", i, e)
		}
	}
	if page.Events[0].Level != "WARN" || page.Events[0].Sequence != "2" || page.Events[0].MonotonicUS != "10" || page.Events[0].DurationUS != 0 || page.Events[0].Module != 1 || page.Events[0].EventCode != 1 || page.Events[0].Arg0 != 1 || page.Events[0].Arg1 != 2 || page.Events[0].RequestID != nil || page.Events[0].RelatedSequence != nil {
		t.Fatalf("first event=%+v", page.Events[0])
	}
	if page.Events[1].Sequence != "18446744073709551615" || page.Events[1].MonotonicUS != "18446744073709551615" || page.Events[1].Level != "INFO" || page.Events[1].Module != 1 || page.Events[1].EventCode != 1 || page.Events[1].Arg0 != 2 || page.Events[1].Arg1 != 5 || page.Events[1].RequestID != nil || page.Events[1].RelatedSequence == nil || *page.Events[1].RelatedSequence != "18446744073709551615" {
		t.Fatalf("max-u64 event=%+v", page.Events[1])
	}
	zero, related := uint32(0), "0"
	second := makeUpload("TEST-DEVICE", "00000000000000000000000000000033", "build-a", 3, 1)
	second.Events[0].RequestID, second.Events[0].RelatedSequence = &zero, &related
	if status, _ := historyPost(t, h, second); status != 200 {
		t.Fatalf("zero POST=%d", status)
	}
	page = historyGet(t, h, "/api/device-debug/events/TEST-DEVICE")
	last := page.Events[len(page.Events)-1]
	if last.RequestID == nil || *last.RequestID != 0 || last.RelatedSequence == nil || *last.RelatedSequence != "0" {
		t.Fatalf("zero-valued optionals=%+v", last)
	}
}

func TestHTTPHistoryMultiBootDedupKeepsOriginalProvenance(t *testing.T) {
	s, _, now := testDebugStore(t, DefaultPolicy())
	h := historyHandler(s)
	first := makeUpload("TEST-DEVICE", "00000000000000000000000000000041", "build-a", 7, 1)
	other := makeUpload("TEST-DEVICE", "00000000000000000000000000000042", "build-a", 7, 1)
	for _, u := range []Upload{first, other} {
		if status, ack := historyPost(t, h, u); status != 200 || ack.StoredCount != 1 {
			t.Fatalf("status=%d ack=%+v", status, ack)
		}
	}
	before := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE")
	if len(before.Events) != 2 || before.Events[0].Sequence != before.Events[1].Sequence || before.Events[0].BootID == before.Events[1].BootID || before.Events[0].IngestionID == before.Events[1].IngestionID {
		t.Fatalf("multi-boot=%+v", before.Events)
	}
	*now = now.Add(time.Minute)
	first.Level, first.CapturedMonotonicUS, first.OldestSequence, first.Overwritten, first.DeliveryLost = "ERROR", "100", "2", "9", "8"
	if status, ack := historyPost(t, h, first); status != 200 || ack.StoredCount != 0 || ack.DeduplicatedCount != 1 {
		t.Fatalf("retry status=%d ack=%+v", status, ack)
	}
	after := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE")
	if len(after.Events) != 2 || after.Events[0] != before.Events[0] || after.Events[1] != before.Events[1] {
		t.Fatalf("duplicate changed history\nbefore=%+v\nafter=%+v", before.Events, after.Events)
	}
}

func TestHTTPHistoryDefaultCursorAndEmptyPrecision(t *testing.T) {
	s, _, _ := testDebugStore(t, DefaultPolicy())
	h := historyHandler(s)
	for batch := 0; batch < 13; batch++ {
		count := 8
		if batch == 12 {
			count = 5
		}
		u := makeUpload("TEST-DEVICE", "00000000000000000000000000000051", "build-a", uint64(batch*8+1), count)
		for i := range u.Events {
			n := batch*8 + i
			u.Events[i].Sequence = strconv.Itoa(n + 1)
			u.Events[i].MonotonicUS = strconv.Itoa(n + 1)
			u.Events[i].Arg0 = int32(n % 3)
		}
		u.CapturedMonotonicUS = u.Events[len(u.Events)-1].MonotonicUS
		if status, ack := historyPost(t, h, u); status != 200 || int(ack.StoredCount) != count {
			t.Fatalf("batch %d status=%d ack=%+v", batch, status, ack)
		}
	}
	first := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE")
	if len(first.Events) != 100 || !first.HasMore {
		t.Fatalf("default page len=%d has_more=%v", len(first.Events), first.HasMore)
	}
	second := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE?after="+first.NextAfter)
	if len(second.Events) != 1 || second.HasMore {
		t.Fatalf("continuation=%+v", second)
	}
	empty := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE?after="+second.NextAfter)
	if len(empty.Events) != 0 || empty.NextAfter != second.NextAfter {
		t.Fatalf("empty cursor=%+v", empty)
	}
	emptyOther := historyGet(t, h, "/api/device-debug/events/OTHER-DEVICE")
	if len(emptyOther.Events) != 0 || emptyOther.NextAfter != "0" || emptyOther.OldestAvailable != "0" || emptyOther.RetentionGap || emptyOther.HasMore {
		t.Fatalf("empty device=%+v", emptyOther)
	}
	maxCursor := "9223372036854775807"
	maxPage := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE?after="+maxCursor)
	if len(maxPage.Events) != 0 || maxPage.NextAfter != maxCursor {
		t.Fatalf("max server cursor=%+v", maxPage)
	}
}

func TestHTTPHistoryPolicyRetentionAndDeviceGaps(t *testing.T) {
	policy := Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 4}
	s, _, now := testDebugStore(t, policy)
	h := historyHandler(s)
	for i := 0; i < 2; i++ {
		u := makeUpload("TEST-DEVICE", "00000000000000000000000000000061", "build-a", uint64(i+1), 1)
		if status, _ := historyPost(t, h, u); status != 200 {
			t.Fatalf("old post=%d", status)
		}
	}
	old := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE")
	oldCursor, _ := strconv.ParseInt(old.Events[0].IngestionID, 10, 64)
	*now = now.Add(time.Hour + time.Second)
	fresh := makeUpload("TEST-DEVICE", "00000000000000000000000000000062", "build-a", 3, 1)
	if status, _ := historyPost(t, h, fresh); status != 200 {
		t.Fatalf("fresh post=%d", status)
	}
	retained := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE?after="+strconv.FormatInt(oldCursor, 10))
	if !retained.RetentionGap || len(retained.Events) != 1 || retained.Events[0].Sequence != "3" || retained.OldestAvailable != retained.Events[0].IngestionID {
		t.Fatalf("retention proof=%+v", retained)
	}
	watermark := old.Events[len(old.Events)-1].IngestionID
	if retained.Events[0].IngestionID == watermark {
		t.Fatalf("watermark %s must be pruned before retained row %s", watermark, retained.Events[0].IngestionID)
	}
	afterWatermark := historyGet(t, h, "/api/device-debug/events/TEST-DEVICE?after="+watermark)
	if afterWatermark.RetentionGap || len(afterWatermark.Events) != 1 || afterWatermark.Events[0] != retained.Events[0] {
		t.Fatalf("exact watermark boundary=%+v", afterWatermark)
	}

	// Cross the global cap while each device is at, but not above, its per-device limit.
	globalPolicy := Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 3}
	s2, _, _ := testDebugStore(t, globalPolicy)
	h2 := historyHandler(s2)
	var otherFirst string
	var testIDs []string
	for i, device := range []string{"OTHER-DEVICE", "TEST-DEVICE", "OTHER-DEVICE", "TEST-DEVICE"} {
		u := makeUpload(device, "00000000000000000000000000000071", "build-a", uint64(i+1), 1)
		if status, _ := historyPost(t, h2, u); status != 200 {
			t.Fatalf("%s post=%d", device, status)
		}
		page := historyGet(t, h2, "/api/device-debug/events/"+device)
		id := page.Events[len(page.Events)-1].IngestionID
		if i == 0 {
			otherFirst = id
		} else if device == "TEST-DEVICE" {
			testIDs = append(testIDs, id)
		}
	}
	other := historyGet(t, h2, "/api/device-debug/events/OTHER-DEVICE")
	if len(other.Events) != 1 || other.Events[0].Sequence != "3" || other.Events[0].IngestionID == otherFirst || other.OldestAvailable != other.Events[0].IngestionID || !other.RetentionGap {
		t.Fatalf("global sole-victim proof: victim=%s page=%+v", otherFirst, other)
	}
	gap := historyGet(t, h2, "/api/device-debug/events/TEST-DEVICE?after="+otherFirst)
	if gap.RetentionGap || len(gap.Events) != 2 || gap.Events[0].Sequence != "2" || gap.Events[1].Sequence != "4" || gap.Events[0].IngestionID != testIDs[0] || gap.Events[1].IngestionID != testIDs[1] || gap.OldestAvailable != testIDs[0] {
		t.Fatalf("unrelated device pruning implied gap at exact other watermark %s: %+v", otherFirst, gap)
	}

	// Incremental per-device eviction is independent of the global cap.
	s3, _, _ := testDebugStore(t, policy)
	h3 := historyHandler(s3)
	for i := 0; i < 3; i++ {
		u := makeUpload("TEST-DEVICE", "00000000000000000000000000000073", "build-a", uint64(i+1), 1)
		if status, _ := historyPost(t, h3, u); status != 200 {
			t.Fatalf("per-device post=%d", status)
		}
	}
	capped := historyGet(t, h3, "/api/device-debug/events/TEST-DEVICE?after=0")
	if len(capped.Events) != 2 || capped.Events[0].Sequence != "2" || capped.Events[1].Sequence != "3" || !capped.RetentionGap {
		t.Fatalf("per-device cap=%+v", capped)
	}
}
