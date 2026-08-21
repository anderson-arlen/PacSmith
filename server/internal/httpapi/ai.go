package httpapi

import (
	"context"
	"net/http"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/ai"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
)

func (s *Server) createPackageAI(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if _, err := s.Library.GetRelease(r.Context(), id); err != nil {
		writeRequestError(w, err)
		return
	}
	if !s.ensureAIReviewReady(w, r) {
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindAi, map[string]string{"release_id": id}, "", id)
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) createGitHubAssetAI(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Owner      string   `json:"github_owner"`
		Repository string   `json:"github_repository"`
		Preferred  string   `json:"preferred_asset"`
		Assets     []string `json:"available_assets"`
	}
	if !decodeJSON(w, r, &body) {
		return
	}
	if strings.TrimSpace(body.Owner) == "" || strings.TrimSpace(body.Repository) == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "github_owner and github_repository are required")
		return
	}
	if len(body.Assets) == 0 {
		writeError(w, http.StatusBadRequest, "bad_request", "available_assets is required")
		return
	}
	if !s.ensureAIReviewReady(w, r) {
		return
	}
	job, err := s.Jobs.Enqueue(r.Context(), jobs.KindAiGitHubAsset, body, "", "")
	if err != nil {
		writeRequestError(w, err)
		return
	}
	acceptedJob(w, job)
}

func (s *Server) listAIModels(w http.ResponseWriter, r *http.Request) {
	provider := strings.TrimSpace(r.URL.Query().Get("provider"))
	if provider == "" {
		row, err := s.DB.Queries.GetLibrarySettings(r.Context())
		if err != nil {
			writeRequestError(w, err)
			return
		}
		provider = row.AiProvider
	}
	provider = ai.NormalizeProvider(provider)
	if !s.ensureAIProviderReady(w, r, provider) {
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	models, err := (&ai.Service{Secrets: s.Secrets}).ListModels(ctx, provider)
	if err != nil {
		writeError(w, http.StatusBadGateway, "provider_error", err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"provider": provider, "models": models})
}

func (s *Server) ensureAIReviewReady(w http.ResponseWriter, r *http.Request) bool {
	settings, configured, ok := s.aiCredentialState(w, r, "")
	if !ok {
		return false
	}
	if err := ai.ValidateConfigured(settings, configured); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", err.Error())
		return false
	}
	return true
}

func (s *Server) ensureAIProviderReady(w http.ResponseWriter, r *http.Request, provider string) bool {
	settings, configured, ok := s.aiCredentialState(w, r, provider)
	if !ok {
		return false
	}
	if err := ai.ValidateProvider(settings.Provider, configured); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", err.Error())
		return false
	}
	return true
}

func (s *Server) aiCredentialState(w http.ResponseWriter, r *http.Request, provider string) (ai.Settings, bool, bool) {
	row, err := s.DB.Queries.GetLibrarySettings(r.Context())
	if err != nil {
		writeRequestError(w, err)
		return ai.Settings{}, false, false
	}
	settings := ai.SettingsFromStore(row.AiProvider, row.AiModel, row.AiReasoningEffort, row.AiExecutionMode)
	if provider != "" {
		settings.Provider = ai.NormalizeProvider(provider)
	}
	configured := false
	if name := ai.CredentialName(settings.Provider); name != "" && s.Secrets != nil {
		configured, err = s.Secrets.Exists(r.Context(), name)
		if err != nil {
			writeRequestError(w, err)
			return ai.Settings{}, false, false
		}
	}
	return settings, configured, true
}
