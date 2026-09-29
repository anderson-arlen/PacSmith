package sqlite

import (
	"context"
	"database/sql"
	"io/fs"
	"path/filepath"
	"testing"
	"testing/fstest"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestRepositoryOptOutMigration(t *testing.T) {
	ctx := context.Background()
	raw, err := sql.Open("sqlite", dsn(filepath.Join(t.TempDir(), "legacy.db")))
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	if err := applyPragmas(ctx, raw); err != nil {
		t.Fatal(err)
	}
	legacy := fstest.MapFS{}
	entries, err := fs.ReadDir(migrationFiles, "migrations")
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if entry.Name() >= "0018_" {
			continue
		}
		name := "migrations/" + entry.Name()
		body, err := fs.ReadFile(migrationFiles, name)
		if err != nil {
			t.Fatal(err)
		}
		legacy[name] = &fstest.MapFile{Data: body}
	}
	if err := migrate(ctx, raw, legacy); err != nil {
		t.Fatal(err)
	}
	queries := sqlcdb.New(raw)
	for _, id := range []string{"untouched", "opted-out", "manual"} {
		project, err := queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
			ID: id, DisplayName: id, ArchPackageName: id, SourceIdentity: "local:" + id,
			HistoryJson: "[]", CreatedAt: "2026-01-01T00:00:00Z", ModifiedAt: "2026-01-01T00:00:00Z",
		})
		if err != nil {
			t.Fatal(err)
		}
		var publish int64
		if id == "manual" {
			publish = 1
		}
		if _, err := queries.UpdateProjectRepo(ctx, sqlcdb.UpdateProjectRepoParams{
			ID: id, Revision: project.Revision, RepoPublish: publish, ModifiedAt: project.ModifiedAt,
		}); err != nil {
			t.Fatal(err)
		}
		if id != "untouched" {
			if err := queries.UpsertProjectRepoPolicy(ctx, sqlcdb.UpsertProjectRepoPolicyParams{
				ProjectID: id, AutomaticSoak: 0, SoakSecondsOverride: -1,
			}); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := migrate(ctx, raw, migrationFiles); err != nil {
		t.Fatal(err)
	}
	for id, want := range map[string]int64{"untouched": 1, "opted-out": 0, "manual": 1} {
		project, err := queries.GetProject(ctx, id)
		if err != nil || project.RepoPublish != want {
			t.Fatalf("%s publication = %d, want %d: %v", id, project.RepoPublish, want, err)
		}
	}
	policy, err := queries.GetProjectRepoPolicy(ctx, "manual")
	if err != nil || policy.AutomaticSoak != 0 {
		t.Fatalf("saved manual promotion changed: %+v, %v", policy, err)
	}
}
