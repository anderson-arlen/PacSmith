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
		func(context.Context, Job, json.RawMessage, func(string)) (json.RawMessage, error) {
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
