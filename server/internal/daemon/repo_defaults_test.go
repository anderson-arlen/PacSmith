package daemon

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestStartupQueuesInheritedPublication(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "library.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	for _, id := range []string{"inherited", "opted-out", "published"} {
		project, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
			ID: id, DisplayName: id, ArchPackageName: id, SourceIdentity: "local:" + id,
			HistoryJson: "[]", CreatedAt: "2026-01-01T00:00:00Z", ModifiedAt: "2026-01-01T00:00:00Z",
		})
		if err != nil {
			t.Fatal(err)
		}
		if id == "inherited" {
			continue
		}
		var publish int64
		var name string
		if id == "published" {
			publish, name = 1, id
		}
		if _, err := db.Queries.UpdateProjectRepo(ctx, sqlcdb.UpdateProjectRepoParams{
			ID: id, Revision: project.Revision, RepoPublish: publish,
			RepoPublishedPkgname: name, ModifiedAt: project.ModifiedAt,
		}); err != nil {
			t.Fatal(err)
		}
	}
	manager, err := jobs.New(db, filepath.Join(root, "jobs"), nil)
	if err != nil {
		t.Fatal(err)
	}
	d := &Daemon{db: db, jobs: manager}
	if err := d.enqueueUnpublishedProjects(ctx); err != nil {
		t.Fatal(err)
	}
	active, err := manager.Active(ctx, jobs.KindRepositoryDistribution)
	if err != nil || len(active) != 1 || active[0].ProjectID != "inherited" {
		t.Fatalf("startup distribution jobs: %+v, %v", active, err)
	}
}
