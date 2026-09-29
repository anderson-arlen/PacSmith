package acp

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/events"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

const testAgent = `import json,sys
session='server-session'
prompt=None
options=[dict(optionId='allow',kind='allow_once',name='Allow'),dict(optionId='reject',kind='reject_once',name='Reject')]
def send(v): print(json.dumps(v),flush=True)
def reply(i,r): send(dict(jsonrpc='2.0',id=i,result=r))
for line in sys.stdin:
 m=json.loads(line); method=m.get('method'); p=m.get('params',{})
 if method=='initialize': reply(m['id'],dict(protocolVersion=1,agentCapabilities=dict(loadSession=True,promptCapabilities=dict(image=True))))
 elif method in ('session/new','session/load'): reply(m['id'],dict(sessionId=session,configOptions=[]))
 elif method=='session/prompt':
  prompt=m['id']
  send(dict(jsonrpc='2.0',method='session/update',params=dict(sessionId=session,update=dict(sessionUpdate='agent_message_chunk',content=dict(type='text',text='Shared answer')))))
  send(dict(jsonrpc='2.0',id=71,method='session/request_permission',params=dict(sessionId=session,toolCall=dict(toolCallId='build',title='Build the package'),options=options)))
 elif method=='session/cancel': reply(prompt,dict(stopReason='cancelled'))
 elif 'result' in m and m['id']==71: reply(prompt,dict(stopReason='end_turn'))
`

func newTestService(t *testing.T) *Service {
	t.Helper()
	dir := t.TempDir()
	db, err := sqlite.Open(context.Background(), filepath.Join(dir, "db"))
	if err != nil {
		t.Fatal(err)
	}
	s := &Service{DB: db, Directory: filepath.Join(dir, "ai"), Events: events.New()}
	if err := s.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close(); _ = db.Close() })
	path := filepath.Join(dir, "agent.py")
	if err := os.WriteFile(path, []byte(testAgent), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Configure(context.Background(), Settings{Revision: 1, Harness: &Profile{Name: "Test", Executable: "/usr/bin/python3", Arguments: []string{path}}}); err != nil {
		t.Fatal(err)
	}
	return s
}
func eventually(t *testing.T, f func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if f() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("condition not reached")
}
func TestSharedPermissionFirstResponseWins(t *testing.T) {
	s := newTestService(t)
	ctx := context.Background()
	row, err := s.Create(ctx, CreateRequest{})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Review"}}); err != nil {
		t.Fatal(err)
	}
	var permission string
	eventually(t, func() bool {
		p, err := s.Permissions(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if len(p) == 1 {
			permission = p[0].ID
			return true
		}
		return false
	})
	if err := s.Answer(ctx, permission, "invented", "client-a"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("invalid option: %v", err)
	}
	var wg sync.WaitGroup
	results := make(chan error, 2)
	for _, client := range []string{"client-a", "client-b"} {
		wg.Add(1)
		go func() { defer wg.Done(); results <- s.Answer(ctx, permission, "allow", client) }()
	}
	wg.Wait()
	close(results)
	wins, conflicts := 0, 0
	for err := range results {
		if err == nil {
			wins++
		} else if errors.Is(err, ErrConflict) {
			conflicts++
		} else {
			t.Fatal(err)
		}
	}
	if wins != 1 || conflicts != 1 {
		t.Fatalf("wins %d conflicts %d", wins, conflicts)
	}
	eventually(t, func() bool { r, _ := s.Get(ctx, row.ID); return r.Status == "idle" })
	if p, _ := s.Permissions(ctx); len(p) != 0 {
		t.Fatal("resolved permission remains pending")
	}
	entries, err := s.EventsAfter(ctx, row.ID, 0)
	if err != nil {
		t.Fatal(err)
	}
	kinds := map[string]bool{}
	for _, e := range entries {
		kinds[e.Kind] = true
	}
	for _, kind := range []string{"prompt", "update", "permission_resolved", "finished"} {
		if !kinds[kind] {
			t.Fatalf("missing shared event %s", kind)
		}
	}
	decision, err := s.DB.Queries.GetACPPermission(ctx, permission)
	if err != nil || decision.AnsweredBy == "" {
		t.Fatalf("missing decision attribution: %v", err)
	}
}
func TestClientRequestCancellationDoesNotCancelAgent(t *testing.T) {
	s := newTestService(t)
	ctx, cancel := context.WithCancel(context.Background())
	row, err := s.Create(ctx, CreateRequest{})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Review"}}); err != nil {
		t.Fatal(err)
	}
	cancel()
	eventually(t, func() bool { p, _ := s.Permissions(context.Background()); return len(p) == 1 })
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Second"}}); !errors.Is(err, ErrConflict) {
		t.Fatalf("concurrent prompt: %v", err)
	}
}
func TestRestartInvalidatesOldPermissionsAndPreservesHistory(t *testing.T) {
	s := newTestService(t)
	ctx := context.Background()
	row, err := s.Create(ctx, CreateRequest{})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Review"}}); err != nil {
		t.Fatal(err)
	}
	eventually(t, func() bool { p, _ := s.Permissions(ctx); return len(p) == 1 })
	pending, _ := s.Permissions(ctx)
	old := pending[0].ID
	s.Close()
	if err := s.Start(ctx); err != nil {
		t.Fatal(err)
	}
	p, _ := s.Permissions(ctx)
	if len(p) != 0 {
		t.Fatal("stale approval survived restart")
	}
	if err := s.Answer(ctx, old, "allow", "client"); !errors.Is(err, ErrConflict) {
		t.Fatalf("stale answer: %v", err)
	}
	saved, err := s.Get(ctx, row.ID)
	if err != nil || saved.Status != "interrupted" {
		t.Fatalf("restart status: %+v %v", saved, err)
	}
	events, _ := s.EventsAfter(ctx, row.ID, 0)
	if len(events) < 2 {
		t.Fatal("history lost")
	}
	if err := s.Prompt(row.ID, []map[string]any{{"type": "text", "text": "Continue"}}); err != nil {
		t.Fatal(err)
	}
	eventually(t, func() bool { p, _ := s.Permissions(ctx); return len(p) == 1 })
}
func TestSettingsRevisionConflict(t *testing.T) {
	s := newTestService(t)
	if _, err := s.Configure(context.Background(), Settings{Revision: 1}); !errors.Is(err, ErrConflict) {
		t.Fatalf("stale settings accepted: %v", err)
	}
}

func TestAutomaticReviewRunsWithoutDesktop(t *testing.T) {
	s := newTestService(t)
	ctx := context.Background()
	s.Library = &library.Service{DB: s.DB, WorkDir: t.TempDir()}
	_, err := s.DB.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{ID: "project", DisplayName: "Demo", ArchPackageName: "demo", HistoryJson: "[]"})
	if err != nil {
		t.Fatal(err)
	}
	policy := "ai"
	if _, err := s.Library.PatchProject(ctx, "project", library.ProjectPatch{AutoBuildPolicy: &policy}); err != nil {
		t.Fatal(err)
	}
	_, err = s.DB.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{ID: "release", ProjectID: "project", State: "needs-review", SourceType: "deb", SourceSha256: "candidate", ArchPackageName: "demo", ArchPkgrel: 1, BodyJson: `{"buildStatus":"never-built","update":{"lastAutomaticStatus":"ai-pending"}}`})
	if err != nil {
		t.Fatal(err)
	}
	s.reviewPending()
	eventually(t, func() bool { p, _ := s.Permissions(ctx); return len(p) == 1 })
	conversations, err := s.List(ctx, "project")
	if err != nil || len(conversations) != 1 || conversations[0].Automatic != 1 {
		t.Fatalf("automatic conversation: %+v %v", conversations, err)
	}
	s.reviewPending()
	conversations, _ = s.List(ctx, "project")
	if len(conversations) != 1 {
		t.Fatal("automatic review launched twice")
	}
	pending, _ := s.Permissions(ctx)
	if err := s.Answer(ctx, pending[0].ID, "allow", "remote-client"); err != nil {
		t.Fatal(err)
	}
	eventually(t, func() bool {
		r, _ := s.Library.GetRelease(ctx, "release")
		return str(object(r.Document["update"])["lastAutomaticStatus"]) == "paused"
	})
}
