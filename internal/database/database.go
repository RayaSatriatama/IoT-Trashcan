// Package database owns SIGAP's embedded SQLite connection and migrations.
package database

import (
	"context"
	"database/sql"
	"fmt"
	"net/url"
	"strings"

	_ "modernc.org/sqlite"
)

// Open opens an SQLite database at path, enables foreign-key enforcement, and
// applies every embedded migration before returning the ready connection.
func Open(path string) (*sql.DB, error) {
	if strings.TrimSpace(path) == "" {
		return nil, fmt.Errorf("database path is required")
	}

	dsn, err := sqliteDSN(path)
	if err != nil {
		return nil, err
	}
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open sqlite database: %w", err)
	}

	if err := db.PingContext(context.Background()); err != nil {
		db.Close()
		return nil, fmt.Errorf("ping sqlite database: %w", err)
	}
	if err := Migrate(context.Background(), db); err != nil {
		db.Close()
		return nil, err
	}

	return db, nil
}

func sqliteDSN(path string) (string, error) {
	if strings.HasPrefix(path, "file:") {
		uri, err := url.Parse(path)
		if err != nil {
			return "", fmt.Errorf("parse sqlite URI: %w", err)
		}
		query := uri.Query()
		query.Set("_pragma", "foreign_keys(1)")
		uri.RawQuery = query.Encode()
		return uri.String(), nil
	}

	uri := &url.URL{Scheme: "file", Path: path}
	query := uri.Query()
	query.Set("_pragma", "foreign_keys(1)")
	uri.RawQuery = query.Encode()
	return uri.String(), nil
}
