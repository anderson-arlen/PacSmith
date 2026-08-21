package ai

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/secret"
)

type Service struct {
	HTTP    *http.Client
	Secrets secret.Store
	Now     func() time.Time
	ctx     context.Context
	log     func(string)
}

func (s *Service) withContext(ctx context.Context) *Service {
	clone := *s
	clone.ctx = ctx
	return &clone
}

func (s *Service) context() context.Context {
	if s != nil && s.ctx != nil {
		return s.ctx
	}
	return context.Background()
}

func (s *Service) now() time.Time {
	if s != nil && s.Now != nil {
		return s.Now()
	}
	return time.Now()
}

func (s *Service) http() *http.Client {
	if s != nil && s.HTTP != nil {
		return s.HTTP
	}
	return &http.Client{
		Timeout: 0,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
}

func (s *Service) progress(message string) {
	if s != nil && s.log != nil {
		s.log(message + "\n")
	}
}

func (s *Service) bearer(provider string) (string, string, error) {
	name := CredentialName(provider)
	if name == "" {
		return "", "", fmt.Errorf("No AI provider is configured")
	}
	if s.Secrets == nil {
		return "", "", fmt.Errorf("The selected AI provider requires a credential and model ID")
	}
	raw, err := s.Secrets.Get(s.context(), name)
	if err != nil {
		if errors.Is(err, secret.ErrNotFound) {
			return "", "", fmt.Errorf("The selected AI provider requires a credential and model ID")
		}
		return "", "", err
	}
	if provider != ProviderChatGPT {
		token := strings.TrimSpace(string(raw))
		if token == "" {
			return "", "", fmt.Errorf("The selected AI provider requires a credential and model ID")
		}
		return token, "", nil
	}
	session, err := parseChatGPTSession(raw)
	if err != nil {
		return "", "", err
	}
	if session.needsRefresh(s.now()) {
		s.progress("Refreshing PacSmith's ChatGPT session…")
		refreshed, err := s.refreshChatGPT(session)
		if err != nil {
			return "", "", err
		}
		serialized, err := refreshed.serialize()
		if err != nil {
			return "", "", err
		}
		if err := s.Secrets.Set(s.context(), SecretChatGPT, serialized); err != nil {
			return "", "", fmt.Errorf("could not persist refreshed ChatGPT session: %w", err)
		}
		session = refreshed
	}
	return session.AccessToken, session.AccountID, nil
}

type reviewRequest struct {
	settings Settings
	prompt   string
	schema   map[string]any
}

func (s *Service) complete(req reviewRequest) Resolution {
	settings := req.settings
	settings.Provider = NormalizeProvider(settings.Provider)
	if settings.Provider == ProviderNone {
		return failedResolution(settings, "No AI provider is configured", "")
	}
	if strings.TrimSpace(settings.Model) == "" {
		return failedResolution(settings, "The selected AI provider requires a credential and model ID", "")
	}
	bearer, accountID, err := s.bearer(settings.Provider)
	if err != nil {
		return failedResolution(settings, err.Error(), "")
	}
	endpoint := openAIResponsesURL
	switch settings.Provider {
	case ProviderChatGPT:
		endpoint = chatGPTResponsesURL
	case ProviderXAI:
		endpoint = xaiResponsesURL
	}
	body := map[string]any{
		"model": settings.Model,
		"input": requestInput(settings.Provider, req.prompt),
		"text": map[string]any{
			"format": map[string]any{
				"type":   "json_schema",
				"name":   "pacsmith_resolution",
				"strict": true,
				"schema": req.schema,
			},
		},
	}
	for key, value := range requestOptions(settings) {
		body[key] = value
	}
	if settings.Provider == ProviderChatGPT {
		body["stream"] = true
		body["store"] = false
	}
	payload, err := json.Marshal(body)
	if err != nil {
		return failedResolution(settings, err.Error(), "")
	}
	httpReq, err := http.NewRequestWithContext(s.context(), http.MethodPost, endpoint, bytes.NewReader(payload))
	if err != nil {
		return failedResolution(settings, err.Error(), "")
	}
	httpReq.Header.Set("Content-Type", "application/json")
	httpReq.Header.Set("Authorization", "Bearer "+bearer)
	if settings.Provider == ProviderChatGPT {
		httpReq.Header.Set("ChatGPT-Account-ID", accountID)
		httpReq.Header.Set("originator", "pacsmith")
		httpReq.Header.Set("User-Agent", "pacsmith/0.1.0")
		httpReq.Header.Set("OpenAI-Beta", "responses=experimental")
		httpReq.Header.Set("Accept", "text/event-stream")
	}
	s.progress(fmt.Sprintf("Connecting to %s for the single review request…", settings.Provider))
	resp, err := s.http().Do(httpReq)
	if err != nil {
		return failedResolution(settings, fmt.Sprintf("AI provider request failed: %v", err), "")
	}
	defer resp.Body.Close()
	s.progress(fmt.Sprintf("Connected (HTTP %d); model is working…", resp.StatusCode))
	raw, err := io.ReadAll(io.LimitReader(resp.Body, MaxTransportBytes+1))
	if err != nil {
		return failedResolution(settings, fmt.Sprintf("AI provider request failed: %v", err), "")
	}
	if len(raw) > MaxTransportBytes {
		return failedResolution(settings, "The provider response exceeded PacSmith's 16 MiB transport limit", "")
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		providerMessage := providerErrorMessage(raw)
		message := providerMessage
		if message == "" {
			message = fmt.Sprintf("AI provider request failed (HTTP %d)", resp.StatusCode)
		} else {
			message = fmt.Sprintf("AI provider rejected the request (HTTP %d): %s", resp.StatusCode, providerMessage)
		}
		return failedResolution(settings, message, truncatedBody(raw, 64<<10))
	}
	s.progress("Validating the completed response…")
	var text string
	if settings.Provider == ProviderChatGPT {
		state := inspectChatGPTSSE(raw)
		if state.err != "" {
			return failedResolution(settings, "ChatGPT response stream failed: "+state.err, truncatedBody(raw, 64<<10))
		}
		text = state.text
		if text == "" {
			return failedResolution(settings, "ChatGPT returned no response text", truncatedBody(raw, 64<<10))
		}
	} else {
		if len(raw) > MaxOutputBytes {
			return failedResolution(settings, "The structured AI output exceeded PacSmith's 128 KiB limit", "")
		}
		var envelope map[string]any
		if err := json.Unmarshal(raw, &envelope); err != nil {
			return failedResolution(settings, "AI provider returned an invalid response envelope", truncatedBody(raw, 64<<10))
		}
		text = extractResponseText(envelope)
	}
	if len(text) > MaxOutputBytes {
		return failedResolution(settings, "The structured AI output exceeded PacSmith's 128 KiB limit", "")
	}
	return parseResolutionObject(settings, []byte(text))
}
