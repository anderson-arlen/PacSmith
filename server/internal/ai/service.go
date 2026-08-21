package ai

import (
	"context"
	"fmt"
	"strings"
)

type PackageRequest struct {
	Settings Settings
	Document map[string]any
}

type GitHubAssetRequest struct {
	Settings   Settings
	Owner      string
	Repository string
	Preferred  string
	Assets     []string
}

func (s *Service) ResolvePackage(ctx context.Context, req PackageRequest, log func(string)) Resolution {
	run := s.withContext(ctx)
	run.log = log
	run.progress("Preparing a redacted package evidence bundle…")
	run.progress("Preparing bounded package evidence; package binaries are not sent.")
	ctx, cancel := context.WithTimeout(run.context(), Deadline)
	defer cancel()
	run = run.withContext(ctx)
	run.log = log
	evidence, err := compactJSON(PackageEvidence(req.Document))
	if err != nil {
		return failedResolution(req.Settings, err.Error(), "")
	}
	return run.complete(reviewRequest{
		settings: req.Settings,
		prompt:   packagePrompt(evidence),
		schema:   responseSchema(FindingFingerprints(req.Document), true),
	})
}

func (s *Service) ResolveGitHubAsset(ctx context.Context, req GitHubAssetRequest, log func(string)) Resolution {
	run := s.withContext(ctx)
	run.log = log
	run.progress("Preparing a bounded GitHub asset-name catalog…")
	run.progress("Only repository identity, asset names, and system architecture are sent.")
	if strings.TrimSpace(req.Owner) == "" || strings.TrimSpace(req.Repository) == "" {
		return failedResolution(req.Settings, "GitHub owner and repository are required", "")
	}
	if len(req.Assets) == 0 {
		return failedResolution(req.Settings, "No GitHub assets were supplied for AI review", "")
	}
	ctx, cancel := context.WithTimeout(run.context(), Deadline)
	defer cancel()
	run = run.withContext(ctx)
	run.log = log
	evidence, err := compactJSON(GitHubAssetEvidence(req.Owner, req.Repository, req.Preferred, req.Assets))
	if err != nil {
		return failedResolution(req.Settings, err.Error(), "")
	}
	return run.complete(reviewRequest{
		settings: req.Settings,
		prompt:   githubAssetPrompt(evidence),
		schema:   responseSchema(nil, false),
	})
}

func SettingsFromStore(provider, model, effort, mode string) Settings {
	return Settings{
		Provider:        NormalizeProvider(provider),
		Model:           strings.TrimSpace(model),
		ReasoningEffort: strings.TrimSpace(effort),
		ExecutionMode:   strings.TrimSpace(mode),
	}
}

func ValidateProvider(provider string, credentialConfigured bool) error {
	if NormalizeProvider(provider) == ProviderNone {
		return fmt.Errorf("No AI provider is configured")
	}
	if !credentialConfigured {
		return fmt.Errorf("The selected provider requires a PacSmith-configured API credential")
	}
	return nil
}

func ValidateConfigured(settings Settings, credentialConfigured bool) error {
	if err := ValidateProvider(settings.Provider, credentialConfigured); err != nil {
		if settings.Provider == ProviderNone {
			return err
		}
		return fmt.Errorf("The selected AI provider requires a credential and model ID")
	}
	if settings.Model == "" {
		return fmt.Errorf("The selected AI provider requires a credential and model ID")
	}
	if settings.ExecutionMode != "" && settings.ExecutionMode != "standard" && settings.ExecutionMode != "fast" {
		return fmt.Errorf("invalid execution mode")
	}
	return nil
}
