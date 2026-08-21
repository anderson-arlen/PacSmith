package library

import (
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
)

func TestIdentityVariablesSurviveClientSaveDocument(t *testing.T) {
	sha := strings.Repeat("a", 64)
	rel := Release{
		ID:              "rel-affine",
		ProjectID:       "proj-affine",
		ArchPackageName: "affine-bin",
		SourceType:      "appimage",
		SourceSHA256:    sha,
		VendorVersion:   "0.27.4",
		Document: map[string]any{
			"displayName":            "AFFiNE",
			"archPackageName":        "affine-bin",
			"originalSourceFilename": "affine-0.27.4-stable-linux-x64.AppImage",
			"sourceSha256":           sha,
			"sourceType":             "appimage",
			"archPkgrel":             1.0,
			"debian": map[string]any{
				"package":      "affine",
				"version":      "0.27.4",
				"architecture": "amd64",
				"description":  "AFFiNE",
			},
			"acquisition": map[string]any{
				"kind":              "github-release",
				"canonicalIdentity": "github:toeverything/AFFiNE",
			},
			"installMapping": map[string]any{
				"optDirectory":   "affine",
				"appImageOffset": "12345",
			},
		},
	}

	vars := identityVariablesFor(rel)
	for _, snippet := range []string{
		"_PACSMITH_PKGNAME='affine-bin'",
		"_PACSMITH_PKGVER='0.27.4'",
		"_PACSMITH_PKGREL='1'",
		"_PACSMITH_ARCH='x86_64'",
		"_PACSMITH_SOURCE='affine-0.27.4-stable-linux-x64.AppImage'",
		"_PACSMITH_SHA256='" + sha + "'",
		"_PACSMITH_APPIMAGE_OFFSET='12345'",
	} {
		if !strings.Contains(vars, snippet) {
			t.Errorf("pacsmith.vars missing %q\n%s", snippet, vars)
		}
	}
}

func TestIdentityVariablesIncludeImportedIcon(t *testing.T) {
	sha := strings.Repeat("b", 64)
	rel := Release{
		ID:              "rel-code",
		ProjectID:       "proj-code",
		ArchPackageName: "code-bin",
		SourceType:      "deb",
		SourceSHA256:    sha,
		Document: map[string]any{
			"displayName":            "Visual Studio Code",
			"archPackageName":        "code-bin",
			"originalSourceFilename": "code_1.133.0_amd64.deb",
			"sourceSha256":           sha,
			"sourceType":             "deb",
			"archPkgrel":             1.0,
			"debian": map[string]any{
				"package":      "code",
				"version":      "1.133.0",
				"architecture": "amd64",
			},
			"installMapping": map[string]any{
				"icon": map[string]any{
					"sourceKind": "payload",
					"sourcePath": "usr/share/pixmaps/vscode.png",
					"sha256":     strings.Repeat("c", 64),
					"format":     "png",
					"iconName":   "code",
				},
			},
		},
	}
	vars := identityVariablesFor(rel)
	if !strings.Contains(vars, "_PACSMITH_ICON='pacsmith-icon.png'") {
		t.Fatalf("expected icon source in vars:\n%s", vars)
	}
	if recipeFromDocument(rel).InstallMapping.Icon.SourcePath != "usr/share/pixmaps/vscode.png" {
		t.Fatal("sourcePath was not copied into the recipe")
	}
}

func TestUpdateConfigurationFromVendorScripts(t *testing.T) {
	analysis := inspect.Analysis{
		Metadata: inspect.Metadata{
			Package:      "code",
			Architecture: "amd64",
		},
		UpdateCandidates: []string{"https://packages.microsoft.com/keys/microsoft.asc"},
		AptCandidates: []inspect.AptRepositoryCandidate{{
			URI:        "https://packages.microsoft.com/repos/code",
			Suite:      "stable",
			Components: []string{"main"},
			SourcePath: "control/postinst heredoc",
		}},
	}
	update := updateConfigurationJSON(analysis)
	if update["strategy"] != "APT repository" {
		t.Fatalf("strategy %v", update["strategy"])
	}
	if update["url"] != "https://packages.microsoft.com/repos/code" {
		t.Fatalf("url %v", update["url"])
	}
	if update["aptSuite"] != "stable" || update["aptComponent"] != "main" {
		t.Fatalf("suite/component %v %v", update["aptSuite"], update["aptComponent"])
	}
	if update["aptPackageName"] != "code" || update["aptArchitecture"] != "amd64" {
		t.Fatalf("package/arch %v %v", update["aptPackageName"], update["aptArchitecture"])
	}

	document := map[string]any{
		"debian": map[string]any{"package": "code", "architecture": "amd64"},
		"update": map[string]any{"strategy": "manual"},
		"maintainerScripts": []any{
			map[string]any{
				"name": "postinst",
				"contents": "cat > /etc/apt/sources.list.d/vscode.sources << EOF\n" +
					"Types: deb\nURIs: https://packages.microsoft.com/repos/code\n" +
					"Suites: stable\nComponents: main\nEOF\n",
			},
		},
	}
	attachUpdateConfiguration(document)
	filled, ok := mapValue(document, "update")
	if !ok || filled["strategy"] != "APT repository" {
		t.Fatalf("existing release was not repaired: %+v", document["update"])
	}
	if filled["url"] != "https://packages.microsoft.com/repos/code" {
		t.Fatalf("repaired url %v", filled["url"])
	}
}

func TestAnalysisDocumentKeepsInspectedFields(t *testing.T) {
	analysis := inspect.Analysis{
		Metadata: inspect.Metadata{
			Package:      "code",
			Version:      "1.133.0",
			Architecture: "amd64",
			Recommends:   "optional-helper",
			Conflicts:    "code-oss",
			Provides:     "vscode",
			RawFields:    map[string]string{"Package": "code", "X-Vendor": "microsoft"},
		},
		ScriptFindings: []inspect.ScriptFinding{{
			ScriptName:          "postinst",
			Kind:                "apt-repository",
			Summary:             "APT repository handled by PacSmith",
			EvidenceFingerprint: "abc",
			Disposition:         inspect.DispositionHandledByPacSmith,
			Provenance: inspect.FieldProvenance{
				Origin:    inspect.OriginDeterministic,
				Rationale: "Repository configuration is retained for update checks.",
			},
		}},
	}
	raw, err := analysisDocument("code.deb", strings.Repeat("a", 64), "pkgbuild", analysis)
	if err != nil {
		t.Fatal(err)
	}
	var document map[string]any
	if err := json.Unmarshal([]byte(raw), &document); err != nil {
		t.Fatal(err)
	}
	debian, _ := mapValue(document, "debian")
	if stringValue(debian, "recommends") != "optional-helper" || stringValue(debian, "conflicts") != "code-oss" {
		t.Fatalf("debian extra fields: %+v", debian)
	}
	rawFields, _ := mapValue(debian, "rawFields")
	if stringValue(rawFields, "X-Vendor") != "microsoft" {
		t.Fatalf("rawFields %+v", rawFields)
	}
	findings := objectSlice(document["scriptFindings"])
	if len(findings) != 1 || stringValue(findings[0], "disposition") != "handled-by-pacsmith" {
		t.Fatalf("findings %+v", document["scriptFindings"])
	}
	provenance, _ := mapValue(findings[0], "provenance")
	if stringValue(provenance, "origin") != "deterministic" {
		t.Fatalf("finding provenance %+v", provenance)
	}
}

func TestAttachScriptFindingsRepairsMissingDisposition(t *testing.T) {
	document := map[string]any{
		"maintainerScripts": []any{
			map[string]any{
				"name": "postinst",
				"contents": "cat > /etc/apt/sources.list.d/vscode.sources << EOF\n" +
					"Types: deb\nURIs: https://packages.microsoft.com/repos/code\n" +
					"Suites: stable\nComponents: main\nEOF\n",
			},
		},
		"scriptFindings": []any{
			map[string]any{
				"scriptName": "postinst",
				"kind":       "apt-repository",
				"summary":    "Vendor APT repository",
			},
		},
	}
	attachScriptFindings(document)
	findings := objectSlice(document["scriptFindings"])
	if len(findings) != 1 || stringValue(findings[0], "disposition") != "handled-by-pacsmith" {
		t.Fatalf("repaired findings %+v", document["scriptFindings"])
	}
}

func TestGitHubAcquisitionFillsUpdate(t *testing.T) {
	update := map[string]any{"strategy": "Manual"}
	acquisition := map[string]any{
		"kind":              "github-release",
		"canonicalIdentity": "github:owner/repo",
		"githubOwner":       "owner",
		"githubRepository":  "repo",
		"originalUrl":       "https://github.com/owner/repo/releases/download/v1/app.deb",
		"githubReleaseId":   "11",
		"githubAssetId":     "22",
		"publisherDigest":   strings.Repeat("d", 64),
	}
	populateUpdateFromCandidates(update, acquisition, "app", "amd64", "github-release")
	if update["strategy"] != "GitHub releases" {
		t.Fatalf("strategy %v", update["strategy"])
	}
	if update["githubOwner"] != "owner" || update["githubRepository"] != "repo" {
		t.Fatalf("github identity %+v", update)
	}
	if update["url"] != "https://github.com/owner/repo/releases/download/v1/app.deb" {
		t.Fatalf("url %v", update["url"])
	}
}

func TestAttachSigningKeysFromVendorScript(t *testing.T) {
	key, err := os.ReadFile("../pgp/testdata/testkey.asc")
	if err != nil {
		t.Fatal(err)
	}
	document := map[string]any{
		"maintainerScripts": []any{
			map[string]any{"name": "postinst", "contents": string(key)},
		},
		"update": map[string]any{"strategy": "Manual"},
	}
	attachSigningKeys(document)
	update, _ := mapValue(document, "update")
	keys := objectSlice(update["signingKeys"])
	if len(keys) != 1 {
		t.Fatalf("signing keys %+v", update["signingKeys"])
	}
	if jsonSliceLen(keys[0]["fingerprints"]) == 0 {
		t.Fatalf("missing fingerprints %+v", keys[0])
	}
	if stringValue(update, "trustedSigningFingerprint") == "" || stringValue(update, "aptSigningKeyring") == "" {
		t.Fatalf("trusted key fields %+v", update)
	}
	if stringValue(keys[0], "contents") == "" {
		t.Fatal("expected key contents for client materialize")
	}
}
