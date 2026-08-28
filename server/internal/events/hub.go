package events

import (
	"sync"
	"sync/atomic"
)

type Event struct {
	Sequence       uint64   `json:"sequence"`
	Topics         []string `json:"topics"`
	ProjectID      string   `json:"project_id,omitempty"`
	ProjectName    string   `json:"project_name,omitempty"`
	PackageName    string   `json:"package_name,omitempty"`
	ReleaseID      string   `json:"release_id,omitempty"`
	JobID          string   `json:"job_id,omitempty"`
	JobKind        string   `json:"job_kind,omitempty"`
	JobStatus      string   `json:"job_status,omitempty"`
	JobMessage     string   `json:"job_message,omitempty"`
	JobCurrent     int64    `json:"job_current,omitempty"`
	JobTotal       int64    `json:"job_total,omitempty"`
	JobFailedItems int64    `json:"job_failed_items,omitempty"`
	JobPausedItems int64    `json:"job_paused_items,omitempty"`
}

type Subscription struct {
	C      <-chan Event
	cancel func()
}

func (s Subscription) Cancel() {
	if s.cancel != nil {
		s.cancel()
	}
}

type Hub struct {
	sequence atomic.Uint64
	mu       sync.Mutex
	nextID   uint64
	clients  map[uint64]chan Event
}

func New() *Hub {
	return &Hub{clients: map[uint64]chan Event{}}
}

func (h *Hub) Current() uint64 {
	if h == nil {
		return 0
	}
	return h.sequence.Load()
}

func (h *Hub) Subscribe() Subscription {
	if h == nil {
		channel := make(chan Event)
		close(channel)
		return Subscription{C: channel}
	}
	h.mu.Lock()
	id := h.nextID
	h.nextID++
	channel := make(chan Event, 16)
	h.clients[id] = channel
	h.mu.Unlock()
	return Subscription{
		C: channel,
		cancel: func() {
			h.mu.Lock()
			if existing, ok := h.clients[id]; ok {
				delete(h.clients, id)
				close(existing)
			}
			h.mu.Unlock()
		},
	}
}

func (h *Hub) Publish(event Event) Event {
	if h == nil {
		return event
	}
	event.Sequence = h.sequence.Add(1)
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, channel := range h.clients {
		select {
		case channel <- event:
		default:
			for len(channel) > 0 {
				<-channel
			}
			channel <- Event{Sequence: event.Sequence, Topics: []string{"all"}}
		}
	}
	return event
}
