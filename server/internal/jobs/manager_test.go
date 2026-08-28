package jobs

import (
	"context"
	"encoding/json"
	"path/filepath"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

func TestManagerNotifiesJobTransitions(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	manager, err := New(db, filepath.Join(t.TempDir(), "jobs"),
		func(context.Context, Job, json.RawMessage, func(string), func(Progress)) (json.RawMessage, error) {
			return nil, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	transitions := make(chan Job, 3)
	manager.SetObserver(func(job Job) { transitions <- job })
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Stop)
	job, err := manager.Enqueue(ctx, KindBuild, map[string]string{"release_id": "release-1"},
		"project-1", "release-1")
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"queued", "running", "succeeded"}
	for _, status := range want {
		select {
		case transition := <-transitions:
			if transition.ID != job.ID || transition.Status != status ||
				transition.ProjectID != "project-1" || transition.ReleaseID != "release-1" {
				t.Fatalf("unexpected transition: %+v", transition)
			}
		case <-time.After(3 * time.Second):
			t.Fatalf("timed out waiting for %s transition", status)
		}
	}
}

func TestRepositoryDistributionJobsAreDeduplicatedByProject(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	started := make(chan struct{})
	release := make(chan struct{})
	manager, err := New(db, filepath.Join(t.TempDir(), "jobs"),
		func(context.Context, Job, json.RawMessage, func(string), func(Progress)) (json.RawMessage, error) {
			close(started)
			<-release
			return nil, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Stop)
	first, err := manager.Enqueue(ctx, KindRepositoryDistribution, nil, "project-1", "")
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-started:
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for repository job to start")
	}
	active, err := manager.Active(ctx, KindRepositoryDistribution)
	if err != nil {
		t.Fatal(err)
	}
	if len(active) != 1 || active[0].ID != first.ID || active[0].Status != "running" {
		t.Fatalf("active jobs %+v", active)
	}
	second, err := manager.Enqueue(ctx, KindRepositoryDistribution, nil, "project-1", "")
	if err != nil {
		t.Fatal(err)
	}
	if second.ID != first.ID {
		t.Fatalf("duplicate repository job ID = %q, want %q", second.ID, first.ID)
	}
	close(release)
}

func TestManagerPublishesStructuredProgress(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "pacsmith.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	manager, err := New(db, filepath.Join(t.TempDir(), "jobs"),
		func(_ context.Context, _ Job, _ json.RawMessage, _ func(string), progress func(Progress)) (json.RawMessage, error) {
			progress(Progress{Message: "Checking Brave", ProjectID: "brave", Current: 3, Total: 9})
			return nil, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	transitions := make(chan Job, 4)
	manager.SetObserver(func(job Job) { transitions <- job })
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Stop)
	if _, err := manager.Enqueue(ctx, KindUpdateCheck, nil, "", ""); err != nil {
		t.Fatal(err)
	}
	for range 2 {
		<-transitions
	}
	progress := <-transitions
	if progress.Status != "running" || progress.Message != "Checking Brave" ||
		progress.ProjectID != "brave" || progress.Current != 3 || progress.Total != 9 {
		t.Fatalf("progress event %+v", progress)
	}
}
