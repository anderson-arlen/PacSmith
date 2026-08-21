package ai

import (
	"errors"
	"strings"
	"time"
)

const (
	ProviderNone    = "none"
	ProviderChatGPT = "chatgpt"
	ProviderOpenAI  = "openai"
	ProviderXAI     = "xai"

	SecretChatGPT = "chatgpt.session"
	SecretOpenAI  = "openai.api_key"
	SecretXAI     = "xai.api_key"

	Deadline          = 8 * time.Minute
	MaxTransportBytes = 16 << 20
	MaxOutputBytes    = 128 << 10
)

type Settings struct {
	Provider        string
	Model           string
	ReasoningEffort string
	ExecutionMode   string
}

type InformationRequest struct {
	ID       string `json:"id"`
	Kind     string `json:"kind"`
	Argument string `json:"argument"`
	Reason   string `json:"reason"`
}

type FieldChange struct {
	Field     string `json:"field"`
	Value     string `json:"value"`
	Rationale string `json:"rationale"`
}

type FindingResolution struct {
	EvidenceFingerprint string `json:"evidenceFingerprint"`
	Disposition         string `json:"disposition"`
	Summary             string `json:"summary"`
	Rationale           string `json:"rationale"`
}

type Resolution struct {
	Success             bool                 `json:"success"`
	Error               string               `json:"error,omitempty"`
	ErrorDetails        string               `json:"errorDetails,omitempty"`
	Provider            string               `json:"provider"`
	Model               string               `json:"model"`
	InformationRequests []InformationRequest `json:"informationRequests"`
	Changes             []FieldChange        `json:"changes"`
	FindingResolutions  []FindingResolution  `json:"findingResolutions"`
	LifecycleScript     string               `json:"lifecycleScript"`
	Rationale           string               `json:"rationale"`
}

func (r Resolution) Err() error {
	if r.Success {
		return nil
	}
	if strings.TrimSpace(r.Error) == "" {
		return errors.New("AI review failed")
	}
	return errors.New(r.Error)
}

func failedResolution(settings Settings, message, details string) Resolution {
	return Resolution{
		Provider:            settings.Provider,
		Model:               settings.Model,
		Error:               message,
		ErrorDetails:        details,
		InformationRequests: []InformationRequest{},
		Changes:             []FieldChange{},
		FindingResolutions:  []FindingResolution{},
	}
}

func CredentialName(provider string) string {
	switch strings.ToLower(strings.TrimSpace(provider)) {
	case ProviderChatGPT:
		return SecretChatGPT
	case ProviderOpenAI:
		return SecretOpenAI
	case ProviderXAI:
		return SecretXAI
	default:
		return ""
	}
}

func NormalizeProvider(provider string) string {
	switch strings.ToLower(strings.TrimSpace(provider)) {
	case ProviderChatGPT:
		return ProviderChatGPT
	case ProviderOpenAI:
		return ProviderOpenAI
	case ProviderXAI:
		return ProviderXAI
	default:
		return ProviderNone
	}
}
