// Package migrations exposes the SQL migration files embedded in the binary.
package migrations

import "embed"

// Files contains every SQL migration in this directory.
//
//go:embed *.sql
var Files embed.FS
