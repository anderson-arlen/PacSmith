package httpapi

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/events"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestJobEventsIncludeHumanReadablePackageIdentity(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := db.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
		ID: "opaque-project-id", DisplayName: "Parsec", ArchPackageName: "parsec-bin",
		SourceIdentity: "local:parsec", HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
	}); err != nil {
		t.Fatal(err)
	}
	hub := events.New()
	subscription := hub.Subscribe()
	defer subscription.Cancel()
	server := &Server{Config: Config{DB: db, Events: hub}}
	server.publishJob(jobs.Job{
		ID: "opaque-job-id", Kind: jobs.KindBuild, Status: "running",
		ProjectID: "opaque-project-id", ReleaseID: "opaque-release-id",
	})
	got := <-subscription.C
	if got.JobKind != jobs.KindBuild || got.JobStatus != "running" ||
		got.ProjectName != "Parsec" || got.PackageName != "parsec-bin" {
		t.Fatalf("job event %+v", got)
	}
}
