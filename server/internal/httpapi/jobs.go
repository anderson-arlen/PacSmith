package httpapi

import (
	"net/http"
	"strconv"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
)

func (s *Server) getJob(w http.ResponseWriter, r *http.Request) {
	job, err := s.Jobs.Get(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
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
