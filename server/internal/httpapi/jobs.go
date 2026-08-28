package httpapi

import (
	"net/http"
	"strconv"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
)

func (s *Server) listActiveJobs(w http.ResponseWriter, r *http.Request) {
	kind := strings.TrimSpace(r.URL.Query().Get("kind"))
	if kind == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "job kind is required")
		return
	}
	active, err := s.Jobs.Active(r.Context(), kind)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	for index := range active {
		if active[index].ProjectID == "" {
			continue
		}
		if project, projectErr := s.DB.Queries.GetProject(r.Context(), active[index].ProjectID); projectErr == nil {
			active[index].ProjectName = project.DisplayName
			active[index].PackageName = project.ArchPackageName
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"jobs": active})
}

func (s *Server) getJob(w http.ResponseWriter, r *http.Request) {
	job, err := s.Jobs.Get(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if job.ProjectID != "" {
		if project, projectErr := s.DB.Queries.GetProject(r.Context(), job.ProjectID); projectErr == nil {
			job.ProjectName = project.DisplayName
			job.PackageName = project.ArchPackageName
		}
	}
	writeJSON(w, http.StatusOK, job)
}

func (s *Server) getJobLog(w http.ResponseWriter, r *http.Request) {
	after, _ := strconv.ParseInt(r.URL.Query().Get("after"), 10, 64)
	chunk, offset, err := s.Jobs.Log(r.PathValue("id"), after)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if _, err := s.Jobs.Get(r.Context(), r.PathValue("id")); err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"chunk":  chunk,
		"offset": offset,
		"id":     r.PathValue("id"),
	})
}

func (s *Server) cancelJob(w http.ResponseWriter, r *http.Request) {
	if err := s.Jobs.Cancel(r.PathValue("id")); err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "canceling"})
}

func acceptedJob(w http.ResponseWriter, job jobs.Job) {
	writeJSON(w, http.StatusAccepted, map[string]any{"job_id": job.ID})
}
