package acp

import (
	"context"
	"errors"
	"testing"
)

func TestCompletionSnapshotAllowsImmediateFollowUp(t *testing.T) {
	s := newTestService(t)
	ctx := context.Background()
	row, err := s.Create(ctx, CreateRequest{})
	if err != nil {
		t.Fatal(err)
	}
	var after int64
	for turn := 0; turn < 40; turn++ {
		failed := turn%2 != 0
		release := make(chan struct{})
		if err := s.run(row.ID, func(r *runner) (map[string]any, error) {
			<-release
			if failed {
				return nil, errors.New("test failure")
			}
			return map[string]any{"stopReason": "end_turn"}, nil
		}); err != nil {
			t.Fatalf("follow-up turn %d: %v", turn, err)
		}
		snapshot, err := s.Snapshot(ctx, row.ID, after)
		close(release)
		if err != nil {
			t.Fatal(err)
		}
		if snapshot.Conversation.Status != "starting" || len(snapshot.Events) != 0 {
			t.Fatalf("unfinished turn: %+v", snapshot)
		}
		eventually(t, func() bool {
			snapshot, err := s.Snapshot(ctx, row.ID, after)
			if err != nil {
				t.Fatal(err)
			}
			terminal := snapshot.Conversation.Status == "idle" || snapshot.Conversation.Status == "failed"
			if terminal != (len(snapshot.Events) == 1) {
				t.Fatalf("status and completion disagree: %+v", snapshot)
			}
			if !terminal {
				return false
			}
			wantKind, wantStatus := "finished", "idle"
			if failed {
				wantKind, wantStatus = "error", "failed"
			}
			if snapshot.Events[0].Kind != wantKind || snapshot.Conversation.Status != wantStatus {
				t.Fatalf("wrong completion: %+v", snapshot)
			}
			after = snapshot.Events[0].ID
			return true
		})
	}
}
