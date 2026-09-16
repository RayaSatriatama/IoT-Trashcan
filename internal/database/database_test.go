package database_test

import (
	"context"
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
