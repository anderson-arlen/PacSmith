package ai

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
)

func parseModelIDs(body []byte) ([]string, error) {
	var object map[string]any
	if err := json.Unmarshal(body, &object); err != nil {
		return nil, errors.New("The provider returned an invalid model catalog")
	}
	data, ok := object["data"].([]any)
	if !ok {
		return nil, errors.New("The provider response did not contain a model list")
	}
	seen := map[string]struct{}{}
	var result []string
	for _, entry := range data {
		id := strings.TrimSpace(asString(asObject(entry)["id"]))
		if id == "" {
			continue
		}
		if _, ok := seen[id]; ok {
			continue
		}
		seen[id] = struct{}{}
		result = append(result, id)
	}
	sort.Strings(result)
	if len(result) == 0 {
		return nil, errors.New("The provider returned an empty model catalog")
	}
	return result, nil
}

func parseChatGPTModelIDs(body []byte) ([]string, error) {
	var object map[string]any
	if err := json.Unmarshal(body, &object); err != nil {
		return nil, errors.New("ChatGPT returned an invalid model catalog")
	}
	entries, ok := object["models"].([]any)
	if !ok {
		return nil, errors.New("ChatGPT's response did not contain a model list")
	}
	seen := map[string]struct{}{}
	var result []string
	for _, entry := range entries {
		object := asObject(entry)
		visibility := asString(object["visibility"])
		if visibility != "" && visibility != "list" {
			continue
		}
		_, hasSnake := object["show_in_picker"]
		_, hasCamel := object["showInPicker"]
		if hasSnake || hasCamel {
			flag := object["show_in_picker"]
			if !hasSnake {
				flag = object["showInPicker"]
			}
			if !asBool(flag) {
				continue
			}
		}
		id := strings.TrimSpace(asString(object["slug"]))
		if id == "" {
			id = strings.TrimSpace(asString(object["id"]))
		}
		if id == "" {
			continue
		}
		if _, ok := seen[id]; ok {
			continue
		}
		seen[id] = struct{}{}
		result = append(result, id)
	}
	if len(result) == 0 {
		return nil, errors.New("Your ChatGPT account returned no selectable models")
	}
	return result, nil
}

func (s *Service) ListModels(ctx context.Context, provider string) ([]string, error) {
	run := s.withContext(ctx)
	provider = NormalizeProvider(provider)
	if provider == ProviderNone {
		return nil, errors.New("No AI provider is configured")
	}
	bearer, accountID, err := run.bearer(provider)
	if err != nil {
		return nil, err
	}
	endpoint := openAIModelsURL
	switch provider {
	case ProviderChatGPT:
		endpoint = chatGPTModelsURL
	case ProviderXAI:
		endpoint = xaiModelsURL
	}
	req, err := http.NewRequestWithContext(run.context(), http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+bearer)
	if provider == ProviderChatGPT {
		req.Header.Set("ChatGPT-Account-ID", accountID)
		req.Header.Set("originator", "pacsmith")
		req.Header.Set("User-Agent", "pacsmith/0.1.0")
		req.Header.Set("Accept", "application/json")
	}
	resp, err := run.http().Do(req)
	if err != nil {
		return nil, fmt.Errorf("Could not load provider models: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, fmt.Errorf("Could not load provider models: %w", err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		detail := providerErrorMessage(body)
		if detail == "" {
			detail = fmt.Sprintf("HTTP %d", resp.StatusCode)
		}
		return nil, fmt.Errorf("Could not load provider models: %s", detail)
	}
	if provider == ProviderChatGPT {
		return parseChatGPTModelIDs(body)
	}
	return parseModelIDs(body)
}
