package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/auth"
	"github.com/anderson-arlen/pacsmith/server/internal/events"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
)

const eventHeartbeatInterval = 15 * time.Second

func (s *Server) streamEvents(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeError(w, http.StatusInternalServerError, "stream_unsupported", "streaming is not supported")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	subscription := s.Events.Subscribe()
	defer subscription.Cancel()
	if !writeServerEvent(w, "sync", events.Event{Sequence: s.Events.Current(), Topics: []string{"all"}}) {
		return
	}
	flusher.Flush()

	heartbeat := time.NewTicker(eventHeartbeatInterval)
	defer heartbeat.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case event, open := <-subscription.C:
			if !open || !s.eventPrincipalActive(r) || !writeServerEvent(w, "change", event) {
				return
			}
			flusher.Flush()
		case <-heartbeat.C:
			if !s.eventPrincipalActive(r) {
				return
			}
			if _, err := fmt.Fprint(w, ": keepalive\n\n"); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}

func (s *Server) eventPrincipalActive(r *http.Request) bool {
	principal := auth.PrincipalFrom(r.Context())
	if principal.Kind != auth.KindRemoteClient {
		return principal.IsLocalAdmin()
	}
	client, err := s.DB.Queries.GetClient(r.Context(), principal.ClientID)
	return err == nil && client.Revoked == 0
}

func writeServerEvent(w http.ResponseWriter, name string, event events.Event) bool {
	body, err := json.Marshal(event)
	if err != nil {
		return false
	}
	_, err = fmt.Fprintf(w, "id: %d\nevent: %s\ndata: %s\n\n", event.Sequence, name, body)
	return err == nil
}

func (s *Server) publishJob(job jobs.Job) {
	topics := []string{"jobs"}
	if job.Status == "succeeded" || job.Status == "failed" || job.Status == "interrupted" {
		topics = append(topics, "projects", "repository")
	}
	event := events.Event{
		Topics: topics, ProjectID: job.ProjectID, ReleaseID: job.ReleaseID,
		JobID: job.ID, JobKind: job.Kind, JobStatus: job.Status,
	}
	if job.ProjectID != "" {
		if project, err := s.DB.Queries.GetProject(context.Background(), job.ProjectID); err == nil {
			event.ProjectName = project.DisplayName
			event.PackageName = project.ArchPackageName
		}
	}
	s.Events.Publish(event)
}

func (s *Server) publishMutation(r *http.Request) {
	event := events.Event{}
	path := r.URL.Path
	switch {
	case strings.HasPrefix(path, "/api/v1/credentials"):
		event.Topics = []string{"credentials"}
	case strings.HasPrefix(path, "/api/v1/registrations"), strings.HasPrefix(path, "/api/v1/clients"), path == "/api/v1/server":
		event.Topics = []string{"administration"}
	case path == "/api/v1/settings":
		event.Topics = []string{"settings"}
	case path == "/api/v1/repo" || strings.HasPrefix(path, "/api/v1/repo/"):
		event.Topics = []string{"repository"}
	case path == "/api/v1/cleanup":
		event.Topics = []string{"all"}
	case strings.HasPrefix(path, "/api/v1/jobs/"):
		event.Topics = []string{"jobs"}
	case path == "/api/v1/imports" || strings.HasSuffix(path, "/reanalyze") || strings.HasSuffix(path, "/builds"):
		event.Topics = []string{"jobs"}
	case strings.HasPrefix(path, "/api/v1/projects/") && strings.Contains(path, "/repo"):
		event.Topics = []string{"projects", "repository"}
	case strings.HasPrefix(path, "/api/v1/projects"), strings.HasPrefix(path, "/api/v1/releases"):
		event.Topics = []string{"projects"}
	default:
		return
	}
	event.ProjectID = pathIdentifier(path, "projects")
	event.ReleaseID = pathIdentifier(path, "releases")
	event.JobID = pathIdentifier(path, "jobs")
	s.Events.Publish(event)
}

func pathIdentifier(path, resource string) string {
	prefix := "/api/v1/" + resource + "/"
	if !strings.HasPrefix(path, prefix) {
		return ""
	}
	value := strings.TrimPrefix(path, prefix)
	if index := strings.IndexByte(value, '/'); index >= 0 {
		value = value[:index]
	}
	if value == "" {
		return ""
	}
	return value
}
