package sqlite

import (
	"context"
	"database/sql"
	"io/fs"
	"path/filepath"
	"testing"
	"testing/fstest"
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
	if version != 14 {
		t.Fatalf("migration version %d", version)
	}
	settings, err := again.Queries.GetLibrarySettings(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if settings.BuildParallelism != 1 {
		t.Fatalf("build parallelism %d", settings.BuildParallelism)
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

func TestBuildArtifactMigrationBackfillsLatestSuccessfulBuild(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "pacsmith.db")
	raw, err := sql.Open("sqlite", dsn(path))
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	if err := applyPragmas(ctx, raw); err != nil {
		t.Fatal(err)
	}
	legacyFiles := fstest.MapFS{}
	entries, err := fs.ReadDir(migrationFiles, "migrations")
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if entry.Name() >= "0008_" {
			continue
		}
		name := "migrations/" + entry.Name()
		body, readErr := fs.ReadFile(migrationFiles, name)
		if readErr != nil {
			t.Fatal(readErr)
		}
		legacyFiles[name] = &fstest.MapFile{Data: body}
	}
	if err := migrate(ctx, raw, legacyFiles); err != nil {
		t.Fatal(err)
	}
	now := "2026-08-25T12:00:00.000Z"
	statements := []struct {
		query string
		args  []any
	}{
		{`INSERT INTO artifacts (id, sha256, size_bytes, original_filename, kind, created_at)
		  VALUES (?, ?, 1, 'app-1-1-any.pkg.tar.zst', 'arch_package', ?)`, []any{"artifact", "sha", now}},
		{`INSERT INTO projects (id, display_name, arch_package_name, source_identity, created_at, modified_at)
		  VALUES ('project', 'App', 'app', 'local:app', ?, ?)`, []any{now, now}},
		{`INSERT INTO releases (id, project_id, state, source_type, original_filename, source_sha256,
		  arch_package_name, body_json, created_at, modified_at)
		  VALUES ('release', 'project', 'needs-review', 'deb', 'app.deb', 'source-sha', 'app', '{}', ?, ?)`, []any{now, now}},
		{`INSERT INTO builds (id, release_id, status, started_at, finished_at)
		  VALUES ('older', 'release', 'succeeded', '2026-08-25T10:00:00Z', '2026-08-25T10:01:00Z')`, nil},
		{`INSERT INTO builds (id, release_id, status, started_at, finished_at)
		  VALUES ('newer', 'release', 'succeeded', '2026-08-25T11:00:00Z', '2026-08-25T11:01:00Z')`, nil},
		{`INSERT INTO release_artifacts (release_id, artifact_id, role)
		  VALUES ('release', 'artifact', 'built_package')`, nil},
	}
	for _, statement := range statements {
		if _, err := raw.ExecContext(ctx, statement.query, statement.args...); err != nil {
			t.Fatal(err)
		}
	}
	if err := migrate(ctx, raw, migrationFiles); err != nil {
		t.Fatal(err)
	}
	var buildID string
	if err := raw.QueryRowContext(ctx,
		`SELECT build_id FROM build_artifacts WHERE artifact_id = 'artifact'`).Scan(&buildID); err != nil {
		t.Fatal(err)
	}
	if buildID != "newer" {
		t.Fatalf("backfilled build %q, want newer", buildID)
	}
}
