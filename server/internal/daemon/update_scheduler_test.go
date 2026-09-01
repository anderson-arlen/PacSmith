package daemon

import (
	"bytes"
	"context"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/anderson-arlen/pacsmith/server/internal/updatecheck"
)

func TestScheduledUpdateCheckRunsCleanup(t *testing.T) {
	for _, test := range []struct {
		name      string
		scheduled bool
		collected bool
	}{
		{name: "scheduled", scheduled: true, collected: true},
		{name: "unscheduled", scheduled: false, collected: false},
	} {
		t.Run(test.name, func(t *testing.T) {
			ctx := context.Background()
			root := t.TempDir()
			db, err := sqlite.Open(ctx, filepath.Join(root, "pacsmith.db"))
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = db.Close() })
			store, err := artifact.New(filepath.Join(root, "objects"), filepath.Join(root, "tmp"))
			if err != nil {
				t.Fatal(err)
			}
			registry := &artifact.Registry{DB: db, Store: store}
			orphan, err := registry.Put(ctx, "orphan.bin", "unknown", bytes.NewReader([]byte("orphan")))
			if err != nil {
				t.Fatal(err)
			}
			lib := &library.Service{DB: db, Artifacts: registry,
				WorkDir: filepath.Join(root, "releases")}
			updater := &updatecheck.Service{DB: db, Library: lib, Artifacts: registry}
			handler := JobHandler(lib, nil, updater)
			payload, err := json.Marshal(map[string]bool{"scheduled": test.scheduled})
			if err != nil {
				t.Fatal(err)
			}
			if _, err := handler(ctx, jobs.Job{Kind: jobs.KindUpdateCheck}, payload,
				func(string) {}, func(jobs.Progress) {}); err != nil {
				t.Fatal(err)
			}
			_, err = registry.Get(ctx, orphan.ID)
			if test.collected && err == nil {
				t.Fatal("scheduled update check did not collect orphaned artifact")
			}
			if !test.collected && err != nil {
				t.Fatalf("unscheduled update check collected orphaned artifact: %v", err)
			}
		})
	}
}

func TestScheduledOccurrence(t *testing.T) {
	location := time.FixedZone("test", -7*60*60)
	now := time.Date(2026, time.August, 28, 14, 30, 0, 0, location)
	daily := scheduledOccurrence(now, true, 1, 2, 0)
	if daily.Day() != 28 || daily.Hour() != 2 {
		t.Fatalf("daily occurrence %s", daily)
	}
	weekly := scheduledOccurrence(now, false, 3, 9, 15)
	if weekly.Day() != 26 || weekly.Hour() != 9 || weekly.Minute() != 15 {
		t.Fatalf("weekly occurrence %s", weekly)
	}
}

func TestSchedulerRecognizesCompletedLibraryCheckAfterProgressAttachesRelease(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	manager, err := jobs.New(db, filepath.Join(t.TempDir(), "jobs"),
		func(_ context.Context, _ jobs.Job, _ json.RawMessage, _ func(string), progress func(jobs.Progress)) (json.RawMessage, error) {
			progress(jobs.Progress{ProjectID: "project-1", ReleaseID: "release-1"})
			return nil, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Stop)

	now := time.Now()
	due := now.Add(-time.Minute)
	settings, err := db.Queries.GetLibrarySettings(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Queries.UpdateLibrarySettings(ctx, sqlcdb.UpdateLibrarySettingsParams{
		AiProvider: settings.AiProvider, AiModel: settings.AiModel,
		AiReasoningEffort: settings.AiReasoningEffort, AiExecutionMode: settings.AiExecutionMode,
		AiAutoResolve: settings.AiAutoResolve, UpdatesEnabled: 1, UpdatesDaily: 1,
		UpdatesWeekday: settings.UpdatesWeekday, UpdatesHour: int64(due.Hour()),
		UpdatesMinute: int64(due.Minute()), UpdatesAutoPrepare: settings.UpdatesAutoPrepare,
		RetentionVersions: settings.RetentionVersions, BuildParallelism: settings.BuildParallelism,
		Revision: settings.Revision,
	}); err != nil {
		t.Fatal(err)
	}
	job, err := manager.Enqueue(ctx, jobs.KindUpdateCheck, struct {
		Scheduled bool `json:"scheduled"`
	}{Scheduled: true}, "", "")
	if err != nil {
		t.Fatal(err)
	}
	waitCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	completed, err := jobs.Wait(waitCtx, manager.Get, job.ID)
	if err != nil {
		t.Fatal(err)
	}
	if completed.ReleaseID != "release-1" {
		t.Fatalf("completed job release = %q, want progress-attached release", completed.ReleaseID)
	}

	daemon := &Daemon{db: db, jobs: manager}
	daemon.enqueueScheduledUpdateIfDue(ctx, now)
	count, err := db.Queries.CountJobsByKind(ctx, jobs.KindUpdateCheck)
	if err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("update-check job count = %d, want 1", count)
	}
}

func TestUpdateBatchSummaryNamesFailuresAndPausedBuilds(t *testing.T) {
	result := updatecheck.BatchResult{Failed: 1, Checks: []updatecheck.Result{
		{ProjectName: "Slack", Status: "error", Message: "repository signing key is missing"},
		{ProjectName: "Signal", Status: "update", UpdateAvailable: true, Prepared: true,
			AutomaticStatus: "paused", AutomaticMessage: "vendor lifecycle scripts changed"},
		{ProjectName: "Brave", Status: "update", UpdateAvailable: true, Built: true,
			AutomaticStatus: "built"},
	}}
	summary := updateBatchSummary(result)
	for _, expected := range []string{"2 update(s) found", "1 built automatically",
		"Slack — repository signing key is missing",
		"Signal — vendor lifecycle scripts changed"} {
		if !strings.Contains(summary, expected) {
			t.Fatalf("summary %q does not contain %q", summary, expected)
		}
	}
	if updatePausedCount(result) != 1 {
		t.Fatalf("paused count = %d, want 1", updatePausedCount(result))
	}
}
