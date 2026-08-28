package httpapi

import (
	"errors"
	"net/http"
	"strings"

	githubapi "github.com/anderson-arlen/pacsmith/server/internal/github"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
)

func (s *Server) resolveGitHub(w http.ResponseWriter, r *http.Request) {
	if s.GitHub == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "GitHub service is unavailable")
		return
	}
	var request githubapi.ResolveRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if strings.TrimSpace(request.URL) == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "url is required")
		return
	}
	result, err := s.GitHub.Resolve(r.Context(), request)
	if err != nil {
		if errors.Is(err, secret.ErrUnavailable) {
			writeError(w, http.StatusServiceUnavailable, "unavailable", err.Error())
			return
		}
		writeError(w, http.StatusBadRequest, "github_error", err.Error())
		return
	}
	writeJSON(w, http.StatusOK, result)
}

func (s *Server) createGitHubImport(w http.ResponseWriter, r *http.Request) {
	if s.GitHub == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "GitHub service is unavailable")
		return
	}
	var request githubapi.ImportRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if strings.TrimSpace(request.URL) == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "url is required")
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindGitHubImport, request,
		request.ExistingProjectID, "")
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}
