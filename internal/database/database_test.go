package database_test

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
)

func TestOpenCreatesInitialSchema(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	defer db.Close()

	for _, table := range []string{"locations", "devices", "slots", "readings", "decisions", "events"} {
		var found string
		err := db.QueryRowContext(context.Background(), "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?", table).Scan(&found)
		if err != nil {
			t.Fatalf("table %q was not created: %v", table, err)
		}
	}
}

func TestOpenEnablesForeignKeysOnEveryConnection(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	db.SetMaxOpenConns(2)

	ctx := context.Background()
	conn1, err := db.Conn(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer conn1.Close()
	conn2, err := db.Conn(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer conn2.Close()

	for number, conn := range []*sql.Conn{conn1, conn2} {
		var enabled int
		if err := conn.QueryRowContext(ctx, "PRAGMA foreign_keys").Scan(&enabled); err != nil {
			t.Fatalf("connection %d query pragma: %v", number+1, err)
		}
		if enabled != 1 {
			t.Fatalf("connection %d foreign_keys=%d, want 1", number+1, enabled)
		}
	}

	if _, err := conn2.ExecContext(ctx, `INSERT INTO slots (id, device_id, waste_stream) VALUES ('orphan', 'missing', 'organic')`); err == nil {
		t.Fatal("second connection accepted an invalid foreign-key write")
	}
}

func TestOpenMigrationIsIdempotent(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sigap.db")
	first, err := database.Open(path)
	if err != nil {
		t.Fatalf("first Open() error = %v", err)
	}
	if err := first.Close(); err != nil {
		t.Fatalf("close first database: %v", err)
	}

	second, err := database.Open(path)
	if err != nil {
		t.Fatalf("second Open() error = %v", err)
	}
	defer second.Close()

	var count int
	if err := second.QueryRow("SELECT COUNT(*) FROM schema_migrations WHERE name = '001_initial.sql'").Scan(&count); err != nil {
		t.Fatalf("read migration metadata: %v", err)
	}
	if count != 1 {
		t.Fatalf("migration count = %d, want 1", count)
	}
}

func TestInitialSchemaMatchesSDDContract(t *testing.T) {
	db, err := database.Open(filepath.Join(t.TempDir(), "sigap.db"))
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	defer db.Close()

	tests := []struct {
		table   string
		want    []string
		mustNot []string
	}{
		{
			table: "slots",
			want:  []string{"id", "device_id", "waste_stream", "operational_capacity"},
		},
		{
			table:   "readings",
			want:    []string{"id", "slot_id", "recorded_at", "raw_payload", "fill", "weight", "ogi", "accumulation", "temperature", "sensor_health"},
			mustNot: []string{"fill_pct", "weight_kg", "accumulated_mass", "temperature_c"},
		},
		{
			table:   "decisions",
			want:    []string{"reading_id", "fis_score", "fis_status", "fis_version", "dominant_reason", "safety_state"},
			mustNot: []string{"device_id", "created_at", "detail"},
		},
		{
			table: "events",
			want:  []string{"id", "device_id", "slot_id", "event_type", "severity", "started_at", "ended_at", "metadata"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.table, func(t *testing.T) {
			columns := tableColumns(t, db, tt.table)
			for _, column := range tt.want {
				if !columns[column] {
					t.Errorf("%s is missing required column %q", tt.table, column)
				}
			}
			for _, column := range tt.mustNot {
				if columns[column] {
					t.Errorf("%s retains obsolete column %q", tt.table, column)
				}
			}
		})
	}
}

func tableColumns(t *testing.T, db interface {
	Query(string, ...any) (*sql.Rows, error)
}, table string) map[string]bool {
	t.Helper()
	rows, err := db.Query("PRAGMA table_info(" + table + ")")
	if err != nil {
		t.Fatalf("inspect %s columns: %v", table, err)
	}
	defer rows.Close()

	columns := make(map[string]bool)
	for rows.Next() {
		var cid int
		var name, columnType string
		var notNull, primaryKey int
		var defaultValue any
		if err := rows.Scan(&cid, &name, &columnType, &notNull, &defaultValue, &primaryKey); err != nil {
			t.Fatalf("scan %s column: %v", table, err)
		}
		columns[name] = true
	}
	if err := rows.Err(); err != nil {
		t.Fatalf("iterate %s columns: %v", table, err)
	}
	return columns
}
