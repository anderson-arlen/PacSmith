package httpapi

import (
	"context"
	"net/http"
	"net/http/httptest"
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
		Message: "Building package", Current: 2, Total: 5,
	})
	got := <-subscription.C
	if got.JobKind != jobs.KindBuild || got.JobStatus != "running" ||
		got.ProjectName != "Parsec" || got.PackageName != "parsec-bin" ||
		got.JobMessage != "Building package" || got.JobCurrent != 2 || got.JobTotal != 5 {
		t.Fatalf("job event %+v", got)
	}
}

func TestReleaseMutationEventsTellConnectedClientsToRefresh(t *testing.T) {
	hub := events.New()
	subscription := hub.Subscribe()
	defer subscription.Cancel()
	server := &Server{Config: Config{Events: hub}}
	request := httptest.NewRequest(http.MethodPatch,
		"/api/v1/releases/release-from-mcp/configuration", nil)
	server.publishMutation(request)
	got := <-subscription.C
	if got.ReleaseID != "release-from-mcp" || len(got.Topics) != 1 || got.Topics[0] != "projects" {
		t.Fatalf("release mutation event %+v", got)
	}
}
