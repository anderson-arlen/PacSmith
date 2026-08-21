package sqlite

import (
	"context"
	"path/filepath"
	"testing"
)

func TestOpenMigratesAndIsIdempotent(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "pacsmith.db")
	db, err := Open(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	state, err := db.Queries.GetServerState(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if state.ID != 1 || state.SchemaKind != "pacsmithd" {
		t.Fatalf("server state %+v", state)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}

	again, err := Open(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	defer again.Close()
	var version int64
	if err := again.SQL.QueryRowContext(ctx, `SELECT MAX(version) FROM schema_migrations`).Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != 4 {
		t.Fatalf("migration version %d", version)
	}
	var foreignKeys, journal string
	if err := again.SQL.QueryRowContext(ctx, `PRAGMA foreign_keys`).Scan(&foreignKeys); err != nil {
		t.Fatal(err)
	}
	if foreignKeys != "1" {
		t.Fatalf("foreign_keys=%s", foreignKeys)
	}
	if err := again.SQL.QueryRowContext(ctx, `PRAGMA journal_mode`).Scan(&journal); err != nil {
		t.Fatal(err)
	}
	if journal != "wal" {
		t.Fatalf("journal_mode=%s", journal)
	}
}
