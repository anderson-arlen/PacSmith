package httpapi

import (
	"encoding/json"
	"io"
	"net/http"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
)

func (s *Server) getRepo(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	settings, err := s.Repo.Settings(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, s.repoJSON(settings))
}

func (s *Server) patchRepo(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	var patch repo.SettingsPatch
	if !decodeJSON(w, r, &patch) {
		return
	}
	before, err := s.Repo.Settings(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	var projectIDs []string
	if patch.Enabled != nil && *patch.Enabled && !before.Enabled {
		if s.Jobs == nil {
			writeError(w, http.StatusServiceUnavailable, "unavailable", "job queue is not configured")
			return
		}
		projectIDs, err = s.Repo.PublishedProjectIDs(r.Context())
		if err != nil {
			writeRequestError(w, err)
			return
		}
	}
	settings, err := s.Repo.PatchSettings(r.Context(), patch)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if s.ApplyRepo != nil {
		if err := s.ApplyRepo(settings.ListenConfig()); err != nil {
			writeRequestError(w, err)
			return
		}
	}
	for _, projectID := range projectIDs {
		if _, err := s.Jobs.Enqueue(r.Context(), jobs.KindRepositoryDistribution,
			map[string]string{"project_id": projectID}, projectID, ""); err != nil {
			writeRequestError(w, err)
			return
		}
	}
	writeJSON(w, http.StatusOK, s.repoJSON(settings))
}

func (s *Server) initRepoSigning(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	settings, err := s.Repo.InitSigning(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, s.repoJSON(settings))
}

func (s *Server) getRepoPublicKey(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	body, name, err := s.Repo.PublicKey(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	w.Header().Set("Content-Type", "application/pgp-keys")
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(body)
}

func (s *Server) uploadRepoRoot(w http.ResponseWriter, r *http.Request) {
	s.handleKeyUpload(w, r, true)
}

func (s *Server) uploadRepoCertified(w http.ResponseWriter, r *http.Request) {
	s.handleKeyUpload(w, r, false)
}

func (s *Server) handleKeyUpload(w http.ResponseWriter, r *http.Request, root bool) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	var body struct {
		PublicKey string `json:"public_key"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	key := []byte(strings.TrimSpace(body.PublicKey))
	if len(key) == 0 {
		writeError(w, http.StatusBadRequest, "bad_request", "public_key is required")
		return
	}
	var (
		settings repo.Settings
		err      error
	)
	if root {
		settings, err = s.Repo.UploadRootKey(r.Context(), key)
	} else {
		settings, err = s.Repo.UploadCertifiedKey(r.Context(), key)
	}
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, s.repoJSON(settings))
}

func (s *Server) getRepoBootstrap(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	channel := r.URL.Query().Get("channel")
	if channel == "" {
		channel = repo.ChannelUnstable
	}
	if !repo.ValidChannel(channel) {
		writeError(w, http.StatusBadRequest, "bad_request", "unknown channel")
		return
	}
	settings, err := s.Repo.Settings(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if channel == repo.ChannelStable && !settings.StableEnabled {
		writeError(w, http.StatusBadRequest, "bad_request", "stable channel is disabled")
		return
	}
	if s.RepoBound != nil {
		settings.Bound = s.RepoBound()
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"channel":     channel,
		"script":      repo.RenderBootstrap(settings, channel),
		"fingerprint": settings.Fingerprint,
		"trust_mode":  settings.TrustMode,
	})
}

func (s *Server) getProjectRepo(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	status, err := s.Repo.ProjectView(r.Context(), r.PathValue("id"))
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func (s *Server) patchProjectRepo(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	var patch repo.ProjectPatch
	if !decodeJSON(w, r, &patch) {
		return
	}
	projectID := r.PathValue("id")
	status, err := s.Repo.PatchProjectDeferred(r.Context(), projectID, patch)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	if s.Jobs == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "job queue is not configured")
		return
	}
	if _, err := s.Jobs.Enqueue(r.Context(), jobs.KindRepositoryDistribution,
		map[string]string{"project_id": projectID}, projectID, ""); err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func (s *Server) promoteProjectRepo(w http.ResponseWriter, r *http.Request) {
	if s.Repo == nil {
		writeError(w, http.StatusServiceUnavailable, "unavailable", "repository is not configured")
		return
	}
	var body struct {
		Pkgver string `json:"pkgver"`
		Arch   string `json:"arch"`
	}
	raw, _ := io.ReadAll(io.LimitReader(r.Body, jsonBodyLimit+1))
	_ = r.Body.Close()
	if len(strings.TrimSpace(string(raw))) > 0 {
		if err := jsonUnmarshal(raw, &body); err != nil {
			writeError(w, http.StatusBadRequest, "bad_request", "invalid json")
			return
		}
	}
	status, err := s.Repo.Promote(r.Context(), r.PathValue("id"), body.Pkgver, body.Arch)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func (s *Server) repoJSON(settings repo.Settings) map[string]any {
	if s.RepoBound != nil {
		settings.Bound = s.RepoBound()
	}
	return map[string]any{
		"revision":                settings.Revision,
		"enabled":                 settings.Enabled,
		"listen_hosts":            settings.ListenHosts,
		"listen_port":             settings.ListenPort,
		"advertised_url":          settings.AdvertisedURL,
		"stable_enabled":          settings.StableEnabled,
		"soak_seconds":            settings.SoakSeconds,
		"package_name_prefix":     settings.PackageNamePrefix,
		"trust_mode":              settings.TrustMode,
		"signing_initialized":     settings.SigningInitialized,
		"fingerprint":             settings.Fingerprint,
		"fingerprint_spaced":      settings.FingerprintSpaced,
		"root_fingerprint":        settings.RootFingerprint,
		"root_fingerprint_spaced": settings.RootFingerprintSpaced,
		"certified":               settings.Certified,
		"keyring_version":         settings.KeyringVersion,
		"keyring_package":         settings.KeyringPackage,
		"keyring_url":             settings.KeyringURL,
		"recovery_message":        settings.RecoveryMessage,
		"bound":                   settings.Bound,
		"certification_help":      repo.CertificationHelp(settings),
		"certification_commands":  repo.CertificationCommands(settings),
	}
}

func jsonUnmarshal(raw []byte, dest any) error {
	return json.Unmarshal(raw, dest)
}
