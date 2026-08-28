package httpapi

import (
	"net/http"
	"strconv"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
)

func (s *Server) listProjects(w http.ResponseWriter, r *http.Request) {
	var projects []library.Project
	var err error
	if r.URL.Query().Get("summary") == "1" {
		projects, err = s.Library.ListProjectSummaries(r.Context())
	} else {
		projects, err = s.Library.ListProjects(r.Context())
	}
	if err != nil {
		writeRequestError(w, err)
		return
	}
	var summaryRepoSettings *repo.Settings
	if r.URL.Query().Get("summary") == "1" && s.Repo != nil {
		settings, settingsErr := s.Repo.Settings(r.Context())
		if settingsErr != nil {
			writeRequestError(w, settingsErr)
			return
		}
		summaryRepoSettings = &settings
	}
	encoded := make([]map[string]any, 0, len(projects))
	for _, project := range projects {
		if r.URL.Query().Get("summary") == "1" {
			encoded = append(encoded, encodeProjectSummary(project, summaryRepoSettings))
		} else {
			encoded = append(encoded, s.projectJSON(r, project))
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"projects": encoded})
}

func encodeProjectSummary(project library.Project, settings *repo.Settings) map[string]any {
	out := encodeProject(project)
	prefix := ""
	if settings != nil {
		prefix = settings.PackageNamePrefix
	}
	original := project.ArchPackageName
	if original == "" {
		original = project.DisplayName
	}
	effective, _ := repo.EffectiveName(project.ArchPackageName, original, prefix,
		project.RepoPkgnameOverride)
	out["repository"] = map[string]any{
		"publish":                project.RepoPublish,
		"effective_package_name": effective,
		"published_package_name": project.RepoPublishedPkgname,
	}
	return out
}

func (s *Server) getProject(w http.ResponseWriter, r *http.Request) {
	project, err := s.Library.FindProject(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, s.projectJSON(r, project))
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
	writeJSON(w, http.StatusOK, s.projectJSON(r, project))
}

func (s *Server) deleteProject(w http.ResponseWriter, r *http.Request) {
	if err := s.Library.DeleteProject(r.Context(), r.PathValue("id")); err != nil {
		writeRequestError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) createDiscoveredRelease(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Document map[string]any `json:"document"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	if body.Document == nil {
		writeError(w, http.StatusBadRequest, "bad_request", "document is required")
		return
	}
	release, err := s.Library.CreateDiscoveredRelease(
		r.Context(), r.PathValue("id"), body.Document)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, encodeRelease(release))
}

func (s *Server) getRelease(w http.ResponseWriter, r *http.Request) {
	release, err := s.Library.GetRelease(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, encodeRelease(release))
}

func (s *Server) inspectPayloadFile(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	if path == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "path is required")
		return
	}
	inspection, err := s.Library.InspectPayloadFile(r.Context(), r.PathValue("id"), path)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, inspection)
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

func (s *Server) patchReleaseConfiguration(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Revision      int64          `json:"revision"`
		Configuration map[string]any `json:"configuration"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	if body.Configuration == nil {
		writeError(w, http.StatusBadRequest, "bad_request", "configuration is required")
		return
	}
	release, err := s.Library.PatchReleaseConfiguration(
		r.Context(), r.PathValue("id"), body.Revision, body.Configuration)
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

func (s *Server) deleteReleaseFile(w http.ResponseWriter, r *http.Request) {
	revision, err := strconv.ParseInt(r.URL.Query().Get("revision"), 10, 64)
	if err != nil || revision < 1 {
		writeError(w, http.StatusBadRequest, "bad_request", "revision is required")
		return
	}
	release, err := s.Library.DeleteFile(r.Context(), r.PathValue("id"), r.PathValue("name"), revision)
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
	release, err := s.Library.GetRelease(r.Context(), id)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindReanalyze, map[string]string{"release_id": id}, release.ProjectID, id)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) createBuild(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	release, err := s.Library.GetRelease(r.Context(), id)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	var body struct {
		Automatic bool `json:"automatic"`
	}
	if r.ContentLength != 0 && !decodeJSON(w, r, &body) {
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindBuild, map[string]string{
		"release_id": id,
		"automatic":  strconv.FormatBool(body.Automatic),
	}, release.ProjectID, id)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) projectJSON(r *http.Request, project library.Project) map[string]any {
	out := encodeProject(project)
	if s.Repo != nil && r != nil {
		if status, err := s.Repo.ProjectView(r.Context(), project.ID); err == nil {
			out["repository"] = status
		}
	}
	return out
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
