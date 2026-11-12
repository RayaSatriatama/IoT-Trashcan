package devicedebug

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
)

func testDebugStore(t *testing.T, policy Policy) (*Store, *sql.DB, *time.Time) {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "debug.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if _, err := db.Exec(`INSERT INTO locations(id,name) VALUES ('loc','test'); INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES ('internal-id','loc','TEST-DEVICE','test-only-digest'); INSERT INTO devices(id,location_id,device_code,credential_digest) VALUES ('internal-id-2','loc','OTHER-DEVICE','test-only-digest')`); err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 10, 22, 12, 0, 0, 0, time.UTC)
	s, err := NewStore(db, policy)
	if err != nil {
		t.Fatal(err)
	}
	s.WithClock(func() time.Time { return now })
	return s, db, &now
}

func fixtureUpload(t *testing.T) (Upload, []byte) {
	t.Helper()
	body, err := os.ReadFile(filepath.Join("..", "..", "contracts", "debug", "v1", "fixtures", "http.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(body) != 407 || body[len(body)-1] != '\n' {
		t.Fatalf("fixture storage bytes=%d, expected exactly 407 with final LF", len(body))
	}
	body = body[:len(body)-1]
	upload, err := DecodeUpload(body)
	if err != nil {
		t.Fatal(err)
	}
	return upload, body
}

func makeUpload(device, boot, build string, first uint64, count int) Upload {
	events := make([]DebugEvent, 0, count)
	for i := 0; i < count; i++ {
		seq := first + uint64(i)
		events = append(events, DebugEvent{Sequence: formatU64(seq), MonotonicUS: formatU64(seq), Level: "INFO", Module: 1, EventCode: 1, Arg0: int32(i)})
	}
	return Upload{SchemaVersion: 1, DeviceID: device, BootID: boot, BuildID: build, Level: "INFO", CapturedMonotonicUS: events[len(events)-1].MonotonicUS, OldestSequence: events[0].Sequence, Overwritten: "0", DeliveryLost: "0", Events: events}
}

func bodyFor(t *testing.T, u Upload) []byte {
	t.Helper()
	b, err := json.Marshal(u)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestIngestUsesExactFixtureBytesAndDurableACK(t *testing.T) {
	s, db, _ := testDebugStore(t, DefaultPolicy())
	u, body := fixtureUpload(t)
	ack, err := s.Ingest(context.Background(), u, body)
	if err != nil {
		t.Fatal(err)
	}
	want := sha256.Sum256(body)
	if ack.RequestSHA256 != hex.EncodeToString(want[:]) || ack.StoredCount != 1 || ack.DeduplicatedCount != 0 || ack.Status != "DURABLE" {
		t.Fatalf("ack=%+v", ack)
	}
	ackBytes, err := os.ReadFile(filepath.Join("..", "..", "contracts", "debug", "v1", "fixtures", "ack.json"))
	if err != nil {
		t.Fatal(err)
	}
	var expected UploadAck
	if err := json.Unmarshal(ackBytes, &expected); err != nil {
		t.Fatal(err)
	}
	if ack != expected {
		t.Fatalf("fixture ACK mismatch: got=%+v want=%+v", ack, expected)
	}
	var code, stored string
	var build, level, captured, received string
	if err := db.QueryRow(`SELECT d.device_code,h.sequence_text,h.build_id,h.snapshot_level,h.captured_monotonic_us,h.received_at FROM device_debug_history h JOIN devices d ON d.device_code=h.device_code WHERE h.device_code=?`, u.DeviceID).Scan(&code, &stored, &build, &level, &captured, &received); err != nil {
		t.Fatal(err)
	}
	if code != u.DeviceID || stored != "2" || build != u.BuildID || level != u.Level || captured != u.CapturedMonotonicUS || received != "2026-10-22T12:00:00.000000000Z" {
		t.Fatalf("provenance device=%q seq=%q build=%q level=%q captured=%q received=%q", code, stored, build, level, captured, received)
	}
}

func TestDuplicateKeepsFirstReceiptAndChangedBuildConflicts(t *testing.T) {
	s, db, now := testDebugStore(t, DefaultPolicy())
	u := makeUpload("TEST-DEVICE", "00000000000000000000000000000001", "build-a", 1, 1)
	body := bodyFor(t, u)
	if _, err := s.Ingest(context.Background(), u, body); err != nil {
		t.Fatal(err)
	}
	var receipt string
	if err := db.QueryRow(`SELECT received_at FROM device_debug_history`).Scan(&receipt); err != nil {
		t.Fatal(err)
	}
	*now = now.Add(time.Minute)
	ack, err := s.Ingest(context.Background(), u, body)
	if err != nil {
		t.Fatal(err)
	}
	var after string
	if err := db.QueryRow(`SELECT received_at FROM device_debug_history`).Scan(&after); err != nil {
		t.Fatal(err)
	}
	if receipt != after || ack.StoredCount != 0 || ack.DeduplicatedCount != 1 {
		t.Fatalf("receipt %q -> %q ack=%+v", receipt, after, ack)
	}
	spaceVariant := append([]byte(" "), body...)
	spaceVariant = append(spaceVariant, '\n')
	spaced, err := DecodeUpload(spaceVariant)
	if err != nil {
		t.Fatal(err)
	}
	spacedAck, err := s.Ingest(context.Background(), spaced, spaceVariant)
	if err != nil || spacedAck.StoredCount != 0 || spacedAck.DeduplicatedCount != 1 || spacedAck.RequestSHA256 == ack.RequestSHA256 {
		t.Fatalf("whitespace retry ack=%+v err=%v", spacedAck, err)
	}
	changed := u
	changed.BuildID = "build-b"
	if _, err := s.Ingest(context.Background(), changed, bodyFor(t, changed)); !errors.Is(err, ErrConflict) {
		t.Fatalf("changed build err=%v", err)
	}
}

func TestTypedUploadMismatchAndIdentityAreRejected(t *testing.T) {
	s, db, _ := testDebugStore(t, DefaultPolicy())
	u, body := fixtureUpload(t)
	mismatch := u
	mismatch.BuildID = "other"
	if _, err := s.Ingest(context.Background(), mismatch, body); err == nil {
		t.Fatal("typed/body mismatch accepted")
	}
	unknown := u
	unknown.DeviceID = " TEST-DEVICE"
	if _, err := s.Ingest(context.Background(), unknown, bodyFor(t, unknown)); err == nil {
		t.Fatal("non-exact/unregistered device identity accepted")
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("rows=%d err=%v", count, err)
	}
}

func TestConflictRollsBackPreviouslyInsertedRows(t *testing.T) {
	s, db, _ := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 2})
	boot := "00000000000000000000000000000001"
	if _, err := s.Ingest(context.Background(), makeUpload("TEST-DEVICE", boot, "build-a", 2, 1), bodyFor(t, makeUpload("TEST-DEVICE", boot, "build-a", 2, 1))); err != nil {
		t.Fatal(err)
	}
	bad := makeUpload("TEST-DEVICE", boot, "build-a", 1, 2)
	bad.Events[1].Arg0 = 42
	if _, err := s.Ingest(context.Background(), bad, bodyFor(t, bad)); !errors.Is(err, ErrConflict) {
		t.Fatalf("conflict err=%v", err)
	}
	var rows, watermarks int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&rows); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_watermarks WHERE device_code='TEST-DEVICE'`).Scan(&watermarks); err != nil {
		t.Fatal(err)
	}
	if rows != 1 || watermarks != 0 {
		t.Fatalf("partial transaction rows=%d watermarks=%d", rows, watermarks)
	}
}

func TestQueryRetentionCursorGlobalHolesAndUint64Precision(t *testing.T) {
	s, _, now := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 4})
	boot := "00000000000000000000000000000001"
	a := makeUpload("TEST-DEVICE", boot, "build-a", 18446744073709551615, 1)
	a.Events[0].MonotonicUS = "9007199254740993"
	a.CapturedMonotonicUS = a.Events[0].MonotonicUS
	if _, err := s.Ingest(context.Background(), a, bodyFor(t, a)); err != nil {
		t.Fatal(err)
	}
	b := makeUpload("OTHER-DEVICE", boot, "build-a", 1, 1)
	if _, err := s.Ingest(context.Background(), b, bodyFor(t, b)); err != nil {
		t.Fatal(err)
	}
	p, err := s.Query(context.Background(), "TEST-DEVICE", "0", 100)
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Events) != 1 || p.Events[0].Sequence != "18446744073709551615" || p.Events[0].MonotonicUS != "9007199254740993" || p.RetentionGap {
		t.Fatalf("page=%+v", p)
	}
	*now = now.Add(2 * time.Hour)
	p, err = s.Query(context.Background(), "TEST-DEVICE", "0", 100)
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Events) != 0 || !p.RetentionGap || p.OldestAvailable != "0" {
		t.Fatalf("expired page=%+v", p)
	}
	for _, after := range []string{"00", "-1", "+1", "9223372036854775808", "18446744073709551615"} {
		if _, err := s.Query(context.Background(), "TEST-DEVICE", after, 1); err == nil {
			t.Errorf("accepted cursor %q", after)
		}
	}
	if _, err := s.Query(context.Background(), "TEST-DEVICE", "0", 101); err == nil {
		t.Error("accepted oversized query limit")
	}
}

func TestCapsRetainNewestAndPrunedDuplicateMayBeReaccepted(t *testing.T) {
	s, db, now := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 2})
	boot := "00000000000000000000000000000001"
	u := makeUpload("TEST-DEVICE", boot, "build-a", 1, 2)
	if _, err := s.Ingest(context.Background(), u, bodyFor(t, u)); err != nil {
		t.Fatal(err)
	}
	v := makeUpload("TEST-DEVICE", boot, "build-a", 3, 2)
	if _, err := s.Ingest(context.Background(), v, bodyFor(t, v)); err != nil {
		t.Fatal(err)
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&count); err != nil || count != 2 {
		t.Fatalf("rows=%d err=%v", count, err)
	}
	p, err := s.Query(context.Background(), "TEST-DEVICE", "0", 100)
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Events) != 2 || p.Events[0].Sequence != "3" || !p.RetentionGap {
		t.Fatalf("page=%+v", p)
	}
	*now = now.Add(time.Second)
	if _, err := s.Ingest(context.Background(), u, bodyFor(t, u)); err != nil {
		t.Fatalf("pruned event should not have permanent replay tombstone: %v", err)
	}
}

func TestMixedDedupAndNewBatchSurvivesCaps(t *testing.T) {
	for _, tc := range []struct {
		name   string
		policy Policy
		other  bool
	}{
		{"per-device", Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 4}, false},
		{"global", Policy{Retention: time.Hour, PerDeviceLimit: 3, GlobalLimit: 3}, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s, db, _ := testDebugStore(t, tc.policy)
			boot := "00000000000000000000000000000001"
			seed := makeUpload("TEST-DEVICE", boot, "build-a", 1, 2)
			if _, err := s.Ingest(context.Background(), seed, bodyFor(t, seed)); err != nil {
				t.Fatal(err)
			}
			if tc.other {
				other := makeUpload("OTHER-DEVICE", boot, "build-a", 1, 1)
				if _, err := s.Ingest(context.Background(), other, bodyFor(t, other)); err != nil {
					t.Fatal(err)
				}
			}
			var id int64
			var receipt, content, provenance string
			if err := db.QueryRow(`SELECT ingestion_id,received_at,event_json,snapshot_level||'|'||captured_monotonic_us||'|'||oldest_sequence||'|'||overwritten||'|'||delivery_lost FROM device_debug_history WHERE device_code='TEST-DEVICE' AND sequence_text='1'`).Scan(&id, &receipt, &content, &provenance); err != nil {
				t.Fatal(err)
			}
			batch := makeUpload("TEST-DEVICE", boot, "build-a", 1, 1)
			batch.Events = append(batch.Events, makeUpload("TEST-DEVICE", boot, "build-a", 3, 1).Events[0])
			batch.CapturedMonotonicUS = "3"
			ack, err := s.Ingest(context.Background(), batch, bodyFor(t, batch))
			if err != nil || ack.Status != "DURABLE" || ack.StoredCount != 1 || ack.DeduplicatedCount != 1 {
				t.Fatalf("ack=%+v err=%v", ack, err)
			}
			var total, seq1, seq2, seq3, other int
			if err := db.QueryRow(`SELECT COUNT(*),SUM(device_code='TEST-DEVICE' AND sequence_text='1'),SUM(device_code='TEST-DEVICE' AND sequence_text='2'),SUM(device_code='TEST-DEVICE' AND sequence_text='3'),SUM(device_code='OTHER-DEVICE') FROM device_debug_history`).Scan(&total, &seq1, &seq2, &seq3, &other); err != nil {
				t.Fatal(err)
			}
			var retained int
			if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history WHERE device_code='TEST-DEVICE' AND sequence_text='1'`).Scan(&retained); err != nil {
				t.Fatal(err)
			}
			var keptID int64
			var keptReceipt, keptContent, keptProvenance string
			if retained == 1 {
				if err := db.QueryRow(`SELECT ingestion_id,received_at,event_json,snapshot_level||'|'||captured_monotonic_us||'|'||oldest_sequence||'|'||overwritten||'|'||delivery_lost FROM device_debug_history WHERE device_code='TEST-DEVICE' AND sequence_text='1'`).Scan(&keptID, &keptReceipt, &keptContent, &keptProvenance); err != nil {
					t.Fatal(err)
				}
			}
			var wmA, wmB sql.NullInt64
			if err := db.QueryRow(`SELECT pruned_through FROM device_debug_watermarks WHERE device_code='TEST-DEVICE'`).Scan(&wmA); err != nil {
				t.Fatal(err)
			}
			if tc.other {
				if err := db.QueryRow(`SELECT pruned_through FROM device_debug_watermarks WHERE device_code='OTHER-DEVICE'`).Scan(&wmB); err != nil && !errors.Is(err, sql.ErrNoRows) {
					t.Fatal(err)
				}
			}
			wantOther := 0
			if tc.other {
				wantOther = 1
			}
			wantTotal := 2
			if tc.other {
				wantTotal = 3
			}
			if total != wantTotal || seq1 != 1 || seq2 != 0 || seq3 != 1 || other != wantOther || keptID != id || keptReceipt != receipt || keptContent != content || keptProvenance != provenance || !wmA.Valid || wmA.Int64 != 2 || wmB.Valid {
				t.Fatalf("total=%d seq=%d/%d/%d other=%d identity=%d/%q provenance=%q watermark=%+v/%+v", total, seq1, seq2, seq3, other, keptID, keptReceipt, keptProvenance, wmA, wmB)
			}
		})
	}
}

func TestProtectedDuplicatesAtPerDeviceCapDoNotPruneUnrelatedRows(t *testing.T) {
	for _, tc := range []struct {
		name  string
		count int
		first uint64
	}{
		{"below cap", 1, 2},
		{"at cap with newer duplicate only", 2, 1},
		{"all retained rows protected at cap", 2, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s, db, _ := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 2})
			boot := "00000000000000000000000000000001"
			seed := makeUpload("TEST-DEVICE", boot, "build-a", tc.first, tc.count)
			if _, err := s.Ingest(context.Background(), seed, bodyFor(t, seed)); err != nil {
				t.Fatal(err)
			}
			var before string
			if err := db.QueryRow(`SELECT group_concat(ingestion_id||'|'||sequence_text||'|'||received_at||'|'||event_json, ';') FROM (SELECT * FROM device_debug_history ORDER BY ingestion_id)`).Scan(&before); err != nil {
				t.Fatal(err)
			}
			duplicate := makeUpload("TEST-DEVICE", boot, "build-a", tc.first, tc.count)
			if tc.name == "at cap with newer duplicate only" {
				duplicate = seed
				duplicate.Events = []DebugEvent{seed.Events[1]}
			}
			if ack, err := s.Ingest(context.Background(), duplicate, bodyFor(t, duplicate)); err != nil || ack.DeduplicatedCount != uint8(len(duplicate.Events)) || ack.StoredCount != 0 {
				t.Fatalf("ack=%+v err=%v", ack, err)
			}
			var after, watermarks string
			if err := db.QueryRow(`SELECT group_concat(ingestion_id||'|'||sequence_text||'|'||received_at||'|'||event_json, ';') FROM (SELECT * FROM device_debug_history ORDER BY ingestion_id)`).Scan(&after); err != nil {
				t.Fatal(err)
			}
			if err := db.QueryRow(`SELECT COALESCE(group_concat(device_code||'|'||pruned_through, ';'),'') FROM device_debug_watermarks`).Scan(&watermarks); err != nil {
				t.Fatal(err)
			}
			if after != before || watermarks != "" {
				t.Fatalf("rows changed %q -> %q; watermarks=%q", before, after, watermarks)
			}
		})
	}
}

func TestMixedBatchAtGlobalCapEvictsOnlyEligibleOtherDeviceVictim(t *testing.T) {
	s, db, _ := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 2})
	boot := "00000000000000000000000000000001"
	seedA := makeUpload("TEST-DEVICE", boot, "build-a", 1, 1)
	seedB := makeUpload("OTHER-DEVICE", boot, "build-b", 1, 1)
	for _, seed := range []Upload{seedA, seedB} {
		if _, err := s.Ingest(context.Background(), seed, bodyFor(t, seed)); err != nil {
			t.Fatal(err)
		}
	}
	var idA int64
	var receiptA, contentA, provenanceA string
	if err := db.QueryRow(`SELECT ingestion_id,received_at,event_json,snapshot_level||'|'||captured_monotonic_us||'|'||oldest_sequence||'|'||overwritten||'|'||delivery_lost FROM device_debug_history WHERE device_code='TEST-DEVICE' AND sequence_text='1'`).Scan(&idA, &receiptA, &contentA, &provenanceA); err != nil {
		t.Fatal(err)
	}
	batch := makeUpload("TEST-DEVICE", boot, "build-a", 1, 1)
	batch.Events = append(batch.Events, makeUpload("TEST-DEVICE", boot, "build-a", 2, 1).Events[0])
	batch.CapturedMonotonicUS = "2"
	ack, err := s.Ingest(context.Background(), batch, bodyFor(t, batch))
	if err != nil || ack.Status != "DURABLE" || ack.StoredCount != 1 || ack.DeduplicatedCount != 1 {
		t.Fatalf("ack=%+v err=%v", ack, err)
	}
	var countA, countB, total int
	if err := db.QueryRow(`SELECT COUNT(*),SUM(device_code='TEST-DEVICE'),SUM(device_code='OTHER-DEVICE') FROM device_debug_history`).Scan(&total, &countA, &countB); err != nil {
		t.Fatal(err)
	}
	var keptID int64
	var keptReceipt, keptContent, keptProvenance string
	if err := db.QueryRow(`SELECT ingestion_id,received_at,event_json,snapshot_level||'|'||captured_monotonic_us||'|'||oldest_sequence||'|'||overwritten||'|'||delivery_lost FROM device_debug_history WHERE device_code='TEST-DEVICE' AND sequence_text='1'`).Scan(&keptID, &keptReceipt, &keptContent, &keptProvenance); err != nil {
		t.Fatal(err)
	}
	var newCount, wmA int
	var wmB sql.NullInt64
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history WHERE device_code='TEST-DEVICE' AND sequence_text='2'`).Scan(&newCount); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT pruned_through FROM device_debug_watermarks WHERE device_code='OTHER-DEVICE'`).Scan(&wmB); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_watermarks WHERE device_code='TEST-DEVICE'`).Scan(&wmA); err != nil {
		t.Fatal(err)
	}
	if total != 2 || countA != 2 || countB != 0 || newCount != 1 || keptID != idA || keptReceipt != receiptA || keptContent != contentA || keptProvenance != provenanceA || wmA != 0 || !wmB.Valid || wmB.Int64 != 2 {
		t.Fatalf("rows total/A/B=%d/%d/%d new=%d duplicate=%d/%q provenance=%q watermarks A=%d B=%+v", total, countA, countB, newCount, keptID, keptReceipt, keptProvenance, wmA, wmB)
	}
}

func TestSingleNewEventAtPerDeviceCapEvictsOnlyOldestUnprotectedRow(t *testing.T) {
	s, db, _ := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 4})
	boot := "00000000000000000000000000000001"
	seed := makeUpload("TEST-DEVICE", boot, "build-a", 1, 2)
	if _, err := s.Ingest(context.Background(), seed, bodyFor(t, seed)); err != nil {
		t.Fatal(err)
	}
	newEvent := makeUpload("TEST-DEVICE", boot, "build-a", 3, 1)
	ack, err := s.Ingest(context.Background(), newEvent, bodyFor(t, newEvent))
	if err != nil || ack.Status != "DURABLE" || ack.StoredCount != 1 || ack.DeduplicatedCount != 0 {
		t.Fatalf("ack=%+v err=%v", ack, err)
	}
	var total, seq1, seq2, seq3 int
	if err := db.QueryRow(`SELECT COUNT(*),SUM(sequence_text='1'),SUM(sequence_text='2'),SUM(sequence_text='3') FROM device_debug_history`).Scan(&total, &seq1, &seq2, &seq3); err != nil {
		t.Fatal(err)
	}
	var watermark int64
	if err := db.QueryRow(`SELECT pruned_through FROM device_debug_watermarks WHERE device_code='TEST-DEVICE'`).Scan(&watermark); err != nil {
		t.Fatal(err)
	}
	if total != 2 || seq1 != 0 || seq2 != 1 || seq3 != 1 || watermark != 1 {
		t.Fatalf("rows=%d sequences=%d/%d/%d watermark=%d", total, seq1, seq2, seq3, watermark)
	}
}

func TestInvalidPolicyAndBatchOverDeviceCap(t *testing.T) {
	if _, err := NewStore(nil, DefaultPolicy()); err == nil {
		t.Fatal("nil db accepted")
	}
	for _, p := range []Policy{{}, {Retention: time.Second, PerDeviceLimit: 2, GlobalLimit: 1}} {
		if _, err := NewStore(&sql.DB{}, p); err == nil {
			t.Errorf("invalid policy accepted: %+v", p)
		}
	}
	s, db, _ := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 2})
	u := makeUpload("TEST-DEVICE", "00000000000000000000000000000001", "build-a", 1, 3)
	if _, err := s.Ingest(context.Background(), u, bodyFor(t, u)); err == nil {
		t.Fatal("batch above per-device cap accepted")
	}
	var rows int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&rows); err != nil || rows != 0 {
		t.Fatalf("rows=%d err=%v", rows, err)
	}
}

func TestQueryValidatesCanonicalCursorAndBoundsSignedMaximum(t *testing.T) {
	s, _, _ := testDebugStore(t, DefaultPolicy())
	for _, after := range []string{"00", "01", "+1", "-1", "9223372036854775808", "18446744073709551615"} {
		if _, err := s.Query(context.Background(), "TEST-DEVICE", after, 10); err == nil {
			t.Errorf("accepted after=%q", after)
		}
	}
	if _, err := s.Query(context.Background(), "TEST-DEVICE", "9223372036854775807", 1); err != nil {
		t.Fatalf("signed maximum cursor rejected: %v", err)
	}
}

func TestGlobalCapEvictsOldestAcrossDevicesWithoutInventingDeviceGap(t *testing.T) {
	s, _, _ := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 3, GlobalLimit: 3})
	bootA := "00000000000000000000000000000001"
	bootB := "00000000000000000000000000000002"
	for _, u := range []Upload{
		makeUpload("TEST-DEVICE", bootA, "build-a", 1, 1),
		makeUpload("OTHER-DEVICE", bootB, "build-a", 1, 1),
		makeUpload("TEST-DEVICE", bootB, "build-a", 1, 1),
		makeUpload("TEST-DEVICE", bootA, "build-a", 2, 1),
	} {
		if _, err := s.Ingest(context.Background(), u, bodyFor(t, u)); err != nil {
			t.Fatal(err)
		}
	}
	page, err := s.Query(context.Background(), "TEST-DEVICE", "0", 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(page.Events) != 1 || page.Events[0].BootID != bootB || page.NextAfter != page.Events[0].IngestionID || !page.HasMore || !page.RetentionGap {
		t.Fatalf("first device page=%+v", page)
	}
	page, err = s.Query(context.Background(), "TEST-DEVICE", page.NextAfter, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(page.Events) != 1 || page.Events[0].Sequence != "2" || page.RetentionGap {
		t.Fatalf("continuation page=%+v", page)
	}
	other, err := s.Query(context.Background(), "OTHER-DEVICE", "0", 10)
	if err != nil || len(other.Events) != 1 || other.RetentionGap {
		t.Fatalf("other device page=%+v err=%v", other, err)
	}
}

func TestExpiredIdentityCanBeAcceptedAgainAfterPruning(t *testing.T) {
	s, db, now := testDebugStore(t, Policy{Retention: time.Minute, PerDeviceLimit: 2, GlobalLimit: 2})
	u := makeUpload("TEST-DEVICE", "00000000000000000000000000000001", "build-a", 1, 1)
	if _, err := s.Ingest(context.Background(), u, bodyFor(t, u)); err != nil {
		t.Fatal(err)
	}
	*now = now.Add(2 * time.Minute)
	if _, err := s.Ingest(context.Background(), u, bodyFor(t, u)); err != nil {
		t.Fatalf("expired duplicate did not become a fresh retained insert: %v", err)
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&count); err != nil || count != 1 {
		t.Fatalf("retained rows=%d err=%v", count, err)
	}
}

func TestConflictAfterExpiredIdentityPruneRollsBackRowsAndWatermarks(t *testing.T) {
	s, db, now := testDebugStore(t, Policy{Retention: 3 * time.Minute, PerDeviceLimit: 4, GlobalLimit: 4})
	boot := "00000000000000000000000000000001"
	first := makeUpload("TEST-DEVICE", boot, "build-a", 1, 1)
	if _, err := s.Ingest(context.Background(), first, bodyFor(t, first)); err != nil {
		t.Fatal(err)
	}
	*now = now.Add(2 * time.Minute)
	second := makeUpload("TEST-DEVICE", boot, "build-a", 2, 1)
	if _, err := s.Ingest(context.Background(), second, bodyFor(t, second)); err != nil {
		t.Fatal(err)
	}
	snapshot := func() (string, string) {
		var rows, watermarks string
		if err := db.QueryRow(`SELECT COALESCE(group_concat(row, ';'),'') FROM (SELECT printf('%d|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s',ingestion_id,device_code,boot_id,sequence_text,event_json,build_id,received_at,snapshot_level,captured_monotonic_us,oldest_sequence,overwritten||'|'||delivery_lost) AS row FROM device_debug_history ORDER BY ingestion_id)`).Scan(&rows); err != nil {
			t.Fatal(err)
		}
		if err := db.QueryRow(`SELECT COALESCE(group_concat(row, ';'),'') FROM (SELECT device_code||'|'||pruned_through AS row FROM device_debug_watermarks ORDER BY device_code)`).Scan(&watermarks); err != nil {
			t.Fatal(err)
		}
		return rows, watermarks
	}
	beforeRows, beforeWatermarks := snapshot()
	if beforeRows == "" || beforeWatermarks != "" {
		t.Fatalf("unexpected seed state rows=%q watermarks=%q", beforeRows, beforeWatermarks)
	}
	*now = now.Add(2 * time.Minute)
	var firstReceipt, secondReceipt string
	if err := db.QueryRow(`SELECT received_at FROM device_debug_history WHERE sequence_text='1'`).Scan(&firstReceipt); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT received_at FROM device_debug_history WHERE sequence_text='2'`).Scan(&secondReceipt); err != nil {
		t.Fatal(err)
	}
	cutoff := formatServerTime(now.Add(-3 * time.Minute))
	if firstReceipt > cutoff || secondReceipt <= cutoff {
		t.Fatalf("wrong expiry branch: first=%q second=%q cutoff=%q", firstReceipt, secondReceipt, cutoff)
	}
	conflict := makeUpload("TEST-DEVICE", boot, "build-a", 1, 2)
	conflict.Events[1].Arg0++
	ack, err := s.Ingest(context.Background(), conflict, bodyFor(t, conflict))
	if !errors.Is(err, ErrConflict) || ack.Status != "" {
		t.Fatalf("ack=%+v err=%v", ack, err)
	}
	afterRows, afterWatermarks := snapshot()
	if afterRows != beforeRows || afterWatermarks != beforeWatermarks {
		t.Fatalf("rollback changed state rows=%q -> %q watermarks=%q -> %q", beforeRows, afterRows, beforeWatermarks, afterWatermarks)
	}
}

func TestStorageFailureReturnsNoACK(t *testing.T) {
	s, db, _ := testDebugStore(t, DefaultPolicy())
	db.SetMaxOpenConns(1)
	if _, err := db.Exec(`PRAGMA query_only=ON`); err != nil {
		t.Fatal(err)
	}
	u, body := fixtureUpload(t)
	ack, err := s.Ingest(context.Background(), u, body)
	if !errors.Is(err, ErrStorage) || ack.Status != "" {
		t.Fatalf("failure produced ack=%+v err=%v", ack, err)
	}
}

func TestConcurrentSameIdentityIngestIsAtomic(t *testing.T) {
	s, db, _ := testDebugStore(t, DefaultPolicy())
	u := makeUpload("TEST-DEVICE", "00000000000000000000000000000001", "build-a", 1, 1)
	body := bodyFor(t, u)
	var wait sync.WaitGroup
	acks := make([]UploadAck, 2)
	errs := make([]error, 2)
	for i := range acks {
		wait.Add(1)
		go func(i int) {
			defer wait.Done()
			acks[i], errs[i] = s.Ingest(context.Background(), u, body)
		}(i)
	}
	wait.Wait()
	for i, err := range errs {
		if err != nil {
			t.Fatalf("ingest %d: %v", i, err)
		}
	}
	stored, duplicate := 0, 0
	for _, ack := range acks {
		stored += int(ack.StoredCount)
		duplicate += int(ack.DeduplicatedCount)
	}
	if stored != 1 || duplicate != 1 {
		t.Fatalf("ACK counts stored=%d duplicate=%d: %+v", stored, duplicate, acks)
	}
	var rows int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&rows); err != nil || rows != 1 {
		t.Fatalf("rows=%d err=%v", rows, err)
	}
}

func TestPruningFailureRollsBackBatchAndWatermark(t *testing.T) {
	s, db, _ := testDebugStore(t, Policy{Retention: time.Hour, PerDeviceLimit: 2, GlobalLimit: 2})
	boot := "00000000000000000000000000000001"
	seed := makeUpload("TEST-DEVICE", boot, "build-a", 1, 2)
	if _, err := s.Ingest(context.Background(), seed, bodyFor(t, seed)); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`CREATE TRIGGER fail_debug_prune BEFORE DELETE ON device_debug_history WHEN OLD.sequence_text='1' BEGIN SELECT RAISE(ABORT,'test-only prune failure'); END`); err != nil {
		t.Fatal(err)
	}
	batch := makeUpload("TEST-DEVICE", boot, "build-a", 3, 2)
	ack, err := s.Ingest(context.Background(), batch, bodyFor(t, batch))
	if !errors.Is(err, ErrStorage) || ack.Status != "" {
		t.Fatalf("prune failure ack=%+v err=%v", ack, err)
	}
	var rows, watermarks int
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_history`).Scan(&rows); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM device_debug_watermarks WHERE device_code='TEST-DEVICE'`).Scan(&watermarks); err != nil {
		t.Fatal(err)
	}
	if rows != 2 || watermarks != 0 {
		t.Fatalf("prune failure left rows=%d watermarks=%d", rows, watermarks)
	}
}
