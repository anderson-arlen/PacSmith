package ai

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/secret"
)

type memorySecrets struct {
	mu    sync.Mutex
	items map[string][]byte
}

func (s *memorySecrets) Get(_ context.Context, name string) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	value, ok := s.items[name]
	if !ok {
		return nil, secret.ErrNotFound
	}
	return append([]byte(nil), value...), nil
}

func (s *memorySecrets) Set(_ context.Context, name string, value []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.items == nil {
		s.items = map[string][]byte{}
	}
	s.items[name] = append([]byte(nil), value...)
	return nil
}

func (s *memorySecrets) Delete(_ context.Context, name string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.items, name)
	return nil
}

func (s *memorySecrets) Exists(_ context.Context, name string) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, ok := s.items[name]
	return ok, nil
}

func TestPackageEvidenceOmitsHostAppArmorState(t *testing.T) {
	document := map[string]any{
		"id":              "rel-1",
		"archPackageName": "vendor-bin",
		"sourceType":      "deb",
		"sourceSha256":    "abc",
		"debian":          map[string]any{"package": "vendor", "version": "1.0"},
		"maintainerScripts": []any{
			map[string]any{"name": "postinst", "contents": "#!/bin/sh\necho hi\n"},
		},
		"scriptFindings": []any{
			map[string]any{
				"scriptName":          "postinst",
				"kind":                "network",
				"summary":             "downloads",
				"evidenceFingerprint": "fp1",
				"disposition":         "unresolved",
			},
		},
		"dependencies": []any{
			map[string]any{"rawExpression": "libc6", "archPackage": "glibc", "status": "Resolved"},
		},
		"payload": []any{
			map[string]any{"path": "usr/bin/app", "type": "file", "size": "12", "contentSha256": "aa"},
			map[string]any{"path": "etc/apparmor.d/vendor", "type": "file", "requiresReview": true, "reviewReason": "policy"},
		},
		"installMapping": map[string]any{
			"optDirectory": "vendor-bin",
			"launchers": []any{
				map[string]any{"enabled": true, "commandName": "vendor", "destination": "/usr/bin/vendor"},
			},
		},
		"generatedPkgbuild": "pkgname=vendor-bin",
		"update":            map[string]any{"strategy": "Manual"},
	}
	evidence := PackageEvidence(document)
	policy := asObject(asObject(evidence["targetSystem"])["appArmorPolicy"])
	if !asBool(policy["retainVendorProfiles"]) || asBool(policy["currentInstallationStateRelevant"]) {
		t.Fatalf("apparmor policy %+v", policy)
	}
	if evidence["artifactType"] != "deb" || evidence["archPackage"] != "vendor-bin" {
		t.Fatalf("identity %+v", evidence)
	}
	scripts := asArray(evidence["maintainerScripts"])
	if len(scripts) != 1 || asObject(scripts[0])["sha256"] == "" {
		t.Fatalf("scripts %+v", scripts)
	}
	if asObject(scripts[0])["untrustedContents"] == nil {
		t.Fatal("expected untrusted script contents")
	}
}

func TestParseResolutionRejectsFollowUps(t *testing.T) {
	raw := []byte(`{
		"status":"resolved",
		"informationRequests":[{"id":"1","kind":"file-exists","argument":"/etc/os-release","reason":"host"}],
		"changes":[],
		"findingResolutions":[],
		"lifecycleScript":"",
		"rationale":"x"
	}`)
	got := parseResolutionObject(Settings{Provider: ProviderOpenAI, Model: "gpt"}, raw)
	if got.Success || !strings.Contains(got.Error, "single-request") {
		t.Fatalf("%+v", got)
	}
}

func TestParseModelCatalog(t *testing.T) {
	ids, err := parseModelIDs([]byte(`{"data":[{"id":"b"},{"id":"a"},{"id":"a"}]}`))
	if err != nil || strings.Join(ids, ",") != "a,b" {
		t.Fatalf("%v %v", ids, err)
	}
	chat, err := parseChatGPTModelIDs([]byte(`{"models":[
		{"slug":"gpt-5","visibility":"list","show_in_picker":true},
		{"id":"hidden","show_in_picker":false}
	]}`))
	if err != nil || strings.Join(chat, ",") != "gpt-5" {
		t.Fatalf("%v %v", chat, err)
	}
}

func TestCompleteUsesStoredSecretAndSchema(t *testing.T) {
	var seenAuth, seenModel, seenStream string
	var body map[string]any
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seenAuth = r.Header.Get("Authorization")
		raw, _ := io.ReadAll(r.Body)
		_ = json.Unmarshal(raw, &body)
		seenModel = asString(body["model"])
		if _, ok := body["stream"]; ok {
			seenStream = "stream"
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{
			"output_text": "{\"status\":\"resolved\",\"informationRequests\":[],\"changes\":[{\"field\":\"dependency.0.treatment\",\"value\":\"required\",\"rationale\":\"glibc\"}],\"findingResolutions\":[],\"lifecycleScript\":\"\",\"rationale\":\"ok\"}"
		}`))
	}))
	t.Cleanup(server.Close)
	secrets := &memorySecrets{items: map[string][]byte{SecretOpenAI: []byte("sk-test")}}
	svc := &Service{
		HTTP:    server.Client(),
		Secrets: secrets,
		Now:     func() time.Time { return time.Unix(1_700_000_000, 0) },
	}
	openAIResponsesURL = server.URL
	t.Cleanup(func() { openAIResponsesURL = "https://api.openai.com/v1/responses" })
	resolution := svc.ResolvePackage(context.Background(), PackageRequest{
		Settings: Settings{Provider: ProviderOpenAI, Model: "gpt-4.1"},
		Document: map[string]any{
			"archPackageName": "vendor-bin",
			"dependencies":    []any{map[string]any{"archPackage": "glibc", "status": "Resolved"}},
		},
	}, nil)
	if !resolution.Success {
		t.Fatalf("%+v", resolution)
	}
	if seenAuth != "Bearer sk-test" || seenModel != "gpt-4.1" || seenStream != "" {
		t.Fatalf("auth=%q model=%q stream=%q", seenAuth, seenModel, seenStream)
	}
	format := asObject(asObject(body["text"])["format"])
	if asString(format["type"]) != "json_schema" || asString(format["name"]) != "pacsmith_resolution" {
		t.Fatalf("format %+v", format)
	}
	if len(resolution.Changes) != 1 || resolution.Changes[0].Field != "dependency.0.treatment" {
		t.Fatalf("changes %+v", resolution.Changes)
	}
}

func TestChatGPTRefreshPersistsSession(t *testing.T) {
	header, _ := json.Marshal(map[string]any{"alg": "none", "typ": "JWT"})
	payload, _ := json.Marshal(map[string]any{
		"https://api.openai.com/auth": map[string]any{"chatgpt_account_id": "acct_1"},
	})
	token := base64.RawURLEncoding.EncodeToString(header) + "." +
		base64.RawURLEncoding.EncodeToString(payload) + ".x"
	mux := http.NewServeMux()
	mux.HandleFunc("/oauth/token", func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"access_token":"` + token + `","refresh_token":"new-refresh","expires_in":3600}`))
	})
	mux.HandleFunc("/responses", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer "+token {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = io.WriteString(w, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"{\\\"status\\\":\\\"resolved\\\",\\\"informationRequests\\\":[],\\\"changes\\\":[],\\\"findingResolutions\\\":[],\\\"lifecycleScript\\\":\\\"\\\",\\\"rationale\\\":\\\"ok\\\"}\"}\n")
		_, _ = io.WriteString(w, "data: [DONE]\n")
	})
	server := httptest.NewServer(mux)
	t.Cleanup(server.Close)

	old := chatGPTSession{AccessToken: token, RefreshToken: "old-refresh", ExpiresAtMs: 1, AccountID: "acct_1"}
	serialized, err := old.serialize()
	if err != nil {
		t.Fatal(err)
	}
	secrets := &memorySecrets{items: map[string][]byte{SecretChatGPT: serialized}}
	svc := &Service{
		HTTP:    server.Client(),
		Secrets: secrets,
		Now:     func() time.Time { return time.Unix(1_800_000_000, 0) },
	}
	origToken := chatGPTTokenEndpoint
	origResponses := chatGPTResponsesURL
	chatGPTTokenEndpoint = server.URL + "/oauth/token"
	chatGPTResponsesURL = server.URL + "/responses"
	t.Cleanup(func() {
		chatGPTTokenEndpoint = origToken
		chatGPTResponsesURL = origResponses
	})
	resolution := svc.ResolveGitHubAsset(context.Background(), GitHubAssetRequest{
		Settings:   Settings{Provider: ProviderChatGPT, Model: "gpt-5"},
		Owner:      "acme",
		Repository: "app",
		Assets:     []string{"app-1.0-x86_64.AppImage"},
	}, nil)
	if !resolution.Success {
		t.Fatalf("%+v", resolution)
	}
	stored, err := secrets.Get(context.Background(), SecretChatGPT)
	if err != nil {
		t.Fatal(err)
	}
	session, err := parseChatGPTSession(stored)
	if err != nil {
		t.Fatal(err)
	}
	if session.RefreshToken != "new-refresh" {
		t.Fatalf("session %+v", session)
	}
}
