package httpapi

import (
	"net/http"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/updatecheck"
)

type updateCheckRequest struct {
	ReleaseID string `json:"release_id"`
	Force     bool   `json:"force"`
	Scheduled bool   `json:"scheduled,omitempty"`
}

func (s *Server) inspectRepositoryKey(w http.ResponseWriter, r *http.Request) {
	if s.Updates == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository-key inspection is unavailable")
		return
	}
	var request struct {
		URL string `json:"url"`
	}
	if !decodeJSON(w, r, &request) {
		return
	}
	result, err := s.Updates.InspectRepositoryKey(r.Context(), request.URL)
	if err != nil {
		writeError(w, http.StatusUnprocessableEntity, "repository_key", err.Error())
		return
	}
	writeJSON(w, http.StatusOK, result)
}

func (s *Server) createRemoteImport(w http.ResponseWriter, r *http.Request) {
	var request updatecheck.DirectImportRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindRemoteImport, request,
		request.ExistingProjectID, "")
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) createRepositoryImport(w http.ResponseWriter, r *http.Request) {
	var request updatecheck.RepositoryImportRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindRepositoryImport, request, "", "")
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) createUpdateCheck(w http.ResponseWriter, r *http.Request) {
	var request updateCheckRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	projectID := ""
	if request.ReleaseID != "" {
		release, err := s.Library.GetRelease(r.Context(), request.ReleaseID)
		if err != nil {
			writeRequestError(w, err)
			return
		}
		projectID = release.ProjectID
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindUpdateCheck, request, projectID, request.ReleaseID)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) prepareDiscoveredUpdate(w http.ResponseWriter, r *http.Request) {
	releaseID := r.PathValue("id")
	release, err := s.Library.GetRelease(r.Context(), releaseID)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindUpdatePrepare,
		map[string]string{"release_id": releaseID}, release.ProjectID, releaseID)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}
