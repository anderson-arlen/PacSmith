package jobs

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func TestStartupRecoversDurableQueueAndInterruptedLogs(t *testing.T) {
	ctx := context.Background()
	root := t.TempDir()
	db, err := sqlite.Open(ctx, filepath.Join(root, "library.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var handled atomic.Int64
	manager, err := New(db, filepath.Join(root, "logs"), func(_ context.Context, job Job, payload json.RawMessage, log func(string), _ func(Progress)) (json.RawMessage, error) {
		if job.Kind != KindBuild || string(payload) != `{"release_id":"test"}` {
			return nil, fmt.Errorf("incorrect restored request: %s", payload)
		}
		handled.Add(1)
		log("built\n")
		return nil, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	base := time.Now().UTC().Add(-time.Hour)
	// Exceed the live queue capacity to catch startup that blocks before starting its worker.
	for index := 0; index < 80; index++ {
		_, err := db.Queries.InsertJob(ctx, sqlcdb.InsertJobParams{
			ID: fmt.Sprintf("queued-%02d", index), Kind: KindBuild, Status: "queued",
			PayloadJson: `{"release_id":"test"}`, CreatedAt: base.Add(time.Duration(index) * time.Second).Format(time.RFC3339Nano),
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	_, err = db.Queries.InsertJob(ctx, sqlcdb.InsertJobParams{ID: "old-running", Kind: KindBuild, Status: "running", PayloadJson: `{}`, CreatedAt: base.Format(time.RFC3339Nano)})
	if err != nil {
		t.Fatal(err)
	}
	started := sql.NullString{String: base.Add(time.Minute).Format(time.RFC3339Nano), Valid: true}
	_, err = db.Queries.UpdateJob(ctx, sqlcdb.UpdateJobParams{ID: "old-running", Status: "running", StartedAt: started})
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.appendLog("old-running", "Output from before the restart\n"); err != nil {
		t.Fatal(err)
	}
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Stop)
	waitCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	job, err := Wait(waitCtx, manager.Get, "queued-79")
	if err != nil || job.Status != "succeeded" {
		t.Fatalf("last restored job: %+v, %v", job, err)
	}
	if handled.Load() != 80 {
		t.Fatalf("handled %d queued jobs", handled.Load())
	}
	old, err := manager.Get(ctx, "old-running")
	if err != nil {
		t.Fatal(err)
	}
	if old.Status != "interrupted" || old.StartedAt != started.String || old.FinishedAt == "" {
		t.Fatalf("recovered running job: %+v", old)
	}
	log, offset, err := manager.Log(old.ID, 0)
	if err != nil || !strings.Contains(log, "Output from before") || !strings.Contains(log, "daemon restart") || offset != old.LogOffset {
		t.Fatalf("recovered log %q offset %d: %v", log, offset, err)
	}
	manager.Stop()
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	manager.Stop()
	again, _, err := manager.Log(old.ID, 0)
	if err != nil || again != log || handled.Load() != 80 {
		t.Fatal("recovery repeated completed work or changed retained logs")
	}
}

func TestShutdownPersistsInterruptedOutcome(t *testing.T) {
	ctx := context.Background()
	db, err := sqlite.Open(ctx, filepath.Join(t.TempDir(), "library.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	started := make(chan struct{})
	manager, err := New(db, t.TempDir(), func(ctx context.Context, _ Job, _ json.RawMessage, _ func(string), _ func(Progress)) (json.RawMessage, error) {
		close(started)
		<-ctx.Done()
		return nil, ctx.Err()
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.Start(ctx); err != nil {
		t.Fatal(err)
	}
	job, err := manager.Enqueue(ctx, KindBuild, nil, "", "")
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-started:
	case <-time.After(3 * time.Second):
		t.Fatal("worker did not start")
	}
	manager.Stop()
	finished, err := manager.Get(ctx, job.ID)
	if err != nil || finished.Status != "interrupted" || finished.FinishedAt == "" {
		t.Fatalf("shutdown outcome: %+v, %v", finished, err)
	}
}
