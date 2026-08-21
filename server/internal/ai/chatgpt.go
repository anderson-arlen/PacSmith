package ai

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const chatGPTClientID = "app_EMoamEEZ73f0CkXaXp7hrann"

var (
	chatGPTTokenEndpoint = "https://auth.openai.com/oauth/token"
	chatGPTResponsesURL  = "https://chatgpt.com/backend-api/codex/responses"
	chatGPTModelsURL     = "https://chatgpt.com/backend-api/codex/models?client_version=0.147.0"
	openAIResponsesURL   = "https://api.openai.com/v1/responses"
	openAIModelsURL      = "https://api.openai.com/v1/models"
	xaiResponsesURL      = "https://api.x.ai/v1/responses"
	xaiModelsURL         = "https://api.x.ai/v1/models"
)

type chatGPTSession struct {
	FormatVersion int    `json:"formatVersion"`
	AccessToken   string `json:"accessToken"`
	RefreshToken  string `json:"refreshToken"`
	ExpiresAtMs   int64  `json:"expiresAtMs"`
	AccountID     string `json:"accountId"`
	Email         string `json:"email"`
	PlanType      string `json:"planType"`
}

func (c chatGPTSession) valid() bool {
	return c.AccessToken != "" && c.RefreshToken != "" && c.ExpiresAtMs > 0 && c.AccountID != ""
}

func (c chatGPTSession) needsRefresh(now time.Time) bool {
	return c.ExpiresAtMs <= now.UnixMilli()+60_000
}

func (c chatGPTSession) serialize() ([]byte, error) {
	c.FormatVersion = 1
	return json.Marshal(c)
}

func parseChatGPTSession(raw []byte) (chatGPTSession, error) {
	var session chatGPTSession
	if err := json.Unmarshal(raw, &session); err != nil {
		return chatGPTSession{}, errors.New("PacSmith's saved ChatGPT session is invalid")
	}
	session.populateIdentity()
	if !session.valid() {
		return chatGPTSession{}, errors.New("PacSmith's saved ChatGPT session is incomplete; sign in again")
	}
	return session, nil
}

func (c *chatGPTSession) populateIdentity() {
	payload := decodeJWTPayload(c.AccessToken)
	auth := asObject(payload["https://api.openai.com/auth"])
	profile := asObject(payload["https://api.openai.com/profile"])
	if c.AccountID == "" {
		c.AccountID = asString(auth["chatgpt_account_id"])
	}
	if email := asString(profile["email"]); email != "" {
		c.Email = email
	}
	if plan := asString(auth["chatgpt_plan_type"]); plan != "" {
		c.PlanType = plan
	}
}

func decodeJWTPayload(token string) map[string]any {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return map[string]any{}
	}
	decoded, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		decoded, err = base64.URLEncoding.DecodeString(parts[1])
		if err != nil {
			return map[string]any{}
		}
	}
	var object map[string]any
	if err := json.Unmarshal(decoded, &object); err != nil {
		return map[string]any{}
	}
	return object
}

func parseChatGPTTokenResponse(body []byte, previousRefresh string, now time.Time) (chatGPTSession, error) {
	var object map[string]any
	if err := json.Unmarshal(body, &object); err != nil {
		return chatGPTSession{}, errors.New("OpenAI returned an invalid OAuth token response")
	}
	session := chatGPTSession{
		AccessToken:  asString(object["access_token"]),
		RefreshToken: asString(object["refresh_token"]),
	}
	if session.RefreshToken == "" {
		session.RefreshToken = previousRefresh
	}
	expiresIn := int64(0)
	switch typed := object["expires_in"].(type) {
	case float64:
		expiresIn = int64(typed)
	case json.Number:
		expiresIn, _ = typed.Int64()
	}
	if expiresIn > 0 {
		session.ExpiresAtMs = now.UnixMilli() + expiresIn*1000
	}
	session.populateIdentity()
	if !session.valid() {
		return chatGPTSession{}, errors.New("OpenAI's OAuth response did not contain a usable ChatGPT session")
	}
	return session, nil
}

func (s *Service) refreshChatGPT(session chatGPTSession) (chatGPTSession, error) {
	form := url.Values{}
	form.Set("grant_type", "refresh_token")
	form.Set("refresh_token", session.RefreshToken)
	form.Set("client_id", chatGPTClientID)
	req, err := http.NewRequestWithContext(s.context(), http.MethodPost, chatGPTTokenEndpoint, strings.NewReader(form.Encode()))
	if err != nil {
		return chatGPTSession{}, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := s.http().Do(req)
	if err != nil {
		return chatGPTSession{}, fmt.Errorf("Could not refresh PacSmith's ChatGPT session: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return chatGPTSession{}, fmt.Errorf("Could not refresh PacSmith's ChatGPT session: %w", err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return chatGPTSession{}, fmt.Errorf("Could not refresh PacSmith's ChatGPT session: HTTP %d", resp.StatusCode)
	}
	refreshed, err := parseChatGPTTokenResponse(body, session.RefreshToken, s.now())
	if err != nil {
		return chatGPTSession{}, err
	}
	return refreshed, nil
}
