package events

import "testing"

func TestHubPublishesIncreasingSequences(t *testing.T) {
	hub := New()
	subscription := hub.Subscribe()
	defer subscription.Cancel()

	first := hub.Publish(Event{Topics: []string{"projects"}, ProjectID: "project-1"})
	second := hub.Publish(Event{Topics: []string{"jobs"}, JobID: "job-1"})
	if first.Sequence != 1 || second.Sequence != 2 || hub.Current() != 2 {
		t.Fatalf("unexpected sequences: first=%d second=%d current=%d", first.Sequence, second.Sequence, hub.Current())
	}
	if received := <-subscription.C; received.ProjectID != "project-1" || received.Sequence != 1 {
		t.Fatalf("unexpected event: %+v", received)
	}
	if received := <-subscription.C; received.JobID != "job-1" || received.Sequence != 2 {
		t.Fatalf("unexpected event: %+v", received)
	}
}

func TestSlowSubscriberIsCoalescedToAll(t *testing.T) {
	hub := New()
	subscription := hub.Subscribe()
	defer subscription.Cancel()
	for i := 0; i < 17; i++ {
		hub.Publish(Event{Topics: []string{"projects"}})
	}
	received := <-subscription.C
	if len(received.Topics) != 1 || received.Topics[0] != "all" || received.Sequence != 17 {
		t.Fatalf("unexpected coalesced event: %+v", received)
	}
}
