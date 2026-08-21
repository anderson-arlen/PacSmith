package httpapi

import (
	"net/http"
	"strconv"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
)

func (s *Server) listProjects(w http.ResponseWriter, r *http.Request) {
	projects, err := s.Library.ListProjects(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	encoded := make([]map[string]any, 0, len(projects))
	for _, project := range projects {
		encoded = append(encoded, encodeProject(project))
	}
	writeJSON(w, http.StatusOK, map[string]any{"projects": encoded})
}

func (s *Server) getProject(w http.ResponseWriter, r *http.Request) {
	project, err := s.Library.FindProject(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, encodeProject(project))
}

func (s *Server) patchProject(w http.ResponseWriter, r *http.Request) {
	var patch library.ProjectPatch
	if !decodeJSON(w, r, &patch) {
		return
	}
	project, err := s.Library.PatchProject(r.Context(), r.PathValue("id"), patch)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, encodeProject(project))
}

func (s *Server) deleteProject(w http.ResponseWriter, r *http.Request) {
	if err := s.Library.DeleteProject(r.Context(), r.PathValue("id")); err != nil {
		writeRequestError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) getRelease(w http.ResponseWriter, r *http.Request) {
	release, err := s.Library.GetRelease(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, encodeRelease(release))
}

func (s *Server) putRelease(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Revision int64          `json:"revision"`
		Document map[string]any `json:"document"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	if body.Document == nil {
		writeError(w, http.StatusBadRequest, "bad_request", "document is required")
		return
	}
	release, err := s.Library.SaveRelease(r.Context(), r.PathValue("id"), body.Revision, body.Document)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, encodeRelease(release))
}

func (s *Server) deleteRelease(w http.ResponseWriter, r *http.Request) {
	if err := s.Library.DeleteRelease(r.Context(), r.PathValue("id")); err != nil {
		writeRequestError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) getReleaseFile(w http.ResponseWriter, r *http.Request) {
	contents, contentType, err := s.Library.File(r.Context(), r.PathValue("id"), r.PathValue("name"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	body := []byte(contents)
	if contentType == "" {
		contentType = "text/plain; charset=utf-8"
	}
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(body)
}

func (s *Server) putReleaseFile(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Revision                 int64  `json:"revision"`
		Contents                 string `json:"contents"`
		PkgbuildManuallyModified *bool  `json:"pkgbuildManuallyModified"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	release, err := s.Library.PutFile(r.Context(), r.PathValue("id"), r.PathValue("name"),
		body.Contents, body.Revision, body.PkgbuildManuallyModified)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, encodeRelease(release))
}

func (s *Server) putReleaseIcon(w http.ResponseWriter, r *http.Request) {
	var body struct {
		ArtifactID string `json:"artifact_id"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	if strings.TrimSpace(body.ArtifactID) == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "artifact_id is required")
		return
	}
	release, err := s.Library.SetReleaseIcon(r.Context(), r.PathValue("id"), body.ArtifactID)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, encodeRelease(release))
}

func (s *Server) createImport(w http.ResponseWriter, r *http.Request) {
	var req library.ImportRequest
	if !decodeJSON(w, r, &req) {
		return
	}
	if strings.TrimSpace(req.ArtifactID) == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "artifact_id is required")
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindImport, req, req.ExistingProjectID, "")
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) reanalyze(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if _, err := s.Library.GetRelease(r.Context(), id); err != nil {
		writeRequestError(w, err)
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindReanalyze, map[string]string{"release_id": id}, "", id)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) createBuild(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if _, err := s.Library.GetRelease(r.Context(), id); err != nil {
		writeRequestError(w, err)
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindBuild, map[string]string{"release_id": id}, "", id)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func encodeProject(project library.Project) map[string]any {
	releases := make([]map[string]any, 0, len(project.Releases))
	for _, release := range project.Releases {
		releases = append(releases, encodeRelease(release))
	}
	history := project.History
	if history == nil {
		history = []library.HistoryEntry{}
	}
	return map[string]any{
		"formatVersion":   5,
		"id":              project.ID,
		"revision":        project.Revision,
		"displayName":     project.DisplayName,
		"archPackageName": project.ArchPackageName,
		"vendorName":      project.VendorName,
		"sourceIdentity":  project.SourceIdentity,
		"iconSha256":      project.IconSha256,
		"history":         history,
		"createdAt":       project.CreatedAt,
		"modifiedAt":      project.ModifiedAt,
		"releases":        releases,
	}
}

func encodeRelease(release library.Release) map[string]any {
	out := map[string]any{}
	for key, value := range release.Document {
		if key == "document" {
			continue
		}
		out[key] = value
	}
	out["id"] = release.ID
	out["projectId"] = release.ProjectID
	out["revision"] = release.Revision
	out["state"] = release.State
	out["sourceType"] = release.SourceType
	out["sourceSha256"] = release.SourceSHA256
	out["archPackageName"] = release.ArchPackageName
	out["createdAt"] = release.CreatedAt
	out["modifiedAt"] = release.ModifiedAt
	if _, ok := out["vendorVersion"]; !ok {
		out["vendorVersion"] = release.VendorVersion
	}
	return out
}
