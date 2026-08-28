package github

import (
	"context"
	"io"
	"net/http"
	"regexp"
	"strings"
	"testing"

	"github.com/anderson-arlen/pacsmith/server/internal/secret"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (fn roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return fn(request)
}

func TestResolveUsesDaemonCredential(t *testing.T) {
	store, err := secret.NewFileStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Set(context.Background(), "github.token", []byte("server-token")); err != nil {
		t.Fatal(err)
	}
	locked := secret.NewLockedStore(secret.BackendFile, store)
	client := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if got := request.Header.Get("Authorization"); got != "Bearer server-token" {
			t.Fatalf("Authorization = %q", got)
		}
		body := `[{"id":20,"tag_name":"v2.0.0","draft":false,"prerelease":false,"assets":[{"id":201,"name":"tool_2.0.0_amd64.deb","browser_download_url":"https://github.com/vendor/tool/releases/download/v2.0.0/tool_2.0.0_amd64.deb","digest":"sha256:` + strings.Repeat("b", 64) + `"}]}]`
		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(strings.NewReader(body)),
			Request:    request,
		}, nil
	})}
	service := Service{Secrets: locked, Client: client}
	result, err := service.Resolve(context.Background(), ResolveRequest{
		URL: "https://github.com/vendor/tool", AssetRegex: `tool_.*_amd64\.deb`,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !result.Success || result.Filename != "tool_2.0.0_amd64.deb" {
		t.Fatalf("result %+v", result)
	}
}

func TestParseURL(t *testing.T) {
	owner, repository, tag, asset, err := parseURL(
		"https://github.com/subframe7536/maple-font/releases/download/v7.9/MapleMono-NF-unhinted.zip")
	if err != nil {
		t.Fatal(err)
	}
	if owner != "subframe7536" || repository != "maple-font" || tag != "v7.9" ||
		asset != "MapleMono-NF-unhinted.zip" {
		t.Fatalf("parsed %q %q %q %q", owner, repository, tag, asset)
	}

	for _, value := range []string{
		"http://github.com/vendor/product",
		"https://example.com/vendor/product",
		"https://user:secret@github.com/vendor/product",
		"https://github.com:8443/vendor/product",
		"https://github.com/vendor/product#fragment",
		"https://github.com/vendor",
	} {
		if _, _, _, _, err := parseURL(value); err == nil {
			t.Errorf("parseURL(%q) succeeded", value)
		}
	}
}

func TestSelectRelease(t *testing.T) {
	stableName := "tool-2.0.0-linux-x86_64.tar.gz"
	previewName := "tool-3.0.0-rc1-linux-x86_64.tar.gz"
	releases := []apiRelease{
		{ID: 30, Tag: "v3.0.0-rc1", Prerelease: true,
			Assets: []apiAsset{{ID: 301, Name: previewName, DownloadURL: "https://github.com/preview"}}},
		{ID: 25, Tag: "v2.5.0", Draft: true,
			Assets: []apiAsset{{ID: 251, Name: "tool-2.5.0-linux-x86_64.tar.gz"}}},
		{ID: 20, Tag: "v2.0.0", Assets: []apiAsset{
			{ID: 201, Name: stableName, DownloadURL: "https://github.com/stable",
				Digest: "sha256:" + strings.Repeat("b", 64)},
			{ID: 202, Name: "tool-2.0.0-linux-aarch64.tar.gz"},
		}},
	}
	request := ResolveRequest{CurrentVersion: "1.0.0"}
	pattern := regexp.MustCompile(`tool-.*-linux-x86_64\.tar\.gz`)
	selected, err := selectRelease(releases, request, "vendor", "tool", "",
		pattern.String(), pattern)
	if err != nil {
		t.Fatal(err)
	}
	if !selected.Success || selected.Tag != "v2.0.0" || selected.Filename != stableName ||
		selected.SHA256 != strings.Repeat("b", 64) || !selected.UpdateAvailable {
		t.Fatalf("selected %+v", selected)
	}

	preview, err := selectRelease(releases[:1], request, "vendor", "tool", "",
		pattern.String(), pattern)
	if err != nil {
		t.Fatal(err)
	}
	if !preview.Success || !preview.Prerelease || !strings.Contains(preview.Message, "No matching stable") {
		t.Fatalf("preview %+v", preview)
	}
}

func TestSelectReleaseUsesProviderIdentityBeforeArtifactVersion(t *testing.T) {
	releases := []apiRelease{{ID: 79, Tag: "v7.9", Assets: []apiAsset{{
		ID: 7901, Name: "MapleMono-NF-unhinted.zip", DownloadURL: "https://github.com/maple",
	}}}}
	pattern := regexp.MustCompile(`MapleMono-NF-unhinted\.zip`)
	selected, err := selectRelease(releases, ResolveRequest{
		CurrentVersion: "1.0.0", CurrentReleaseID: 79, CurrentAssetID: 7901,
	}, "subframe7536", "maple-font", "", pattern.String(), pattern)
	if err != nil {
		t.Fatal(err)
	}
	if !selected.Success || selected.UpdateAvailable || selected.DetectedVersion != "7.9" {
		t.Fatalf("selected %+v", selected)
	}
}

func TestSelectReleaseTreatsDifferentAssetInSameReleaseAsUpdate(t *testing.T) {
	releases := []apiRelease{{ID: 79, Tag: "v7.9", Assets: []apiAsset{{
		ID: 7902, Name: "MapleMono-NF-hinted.zip", DownloadURL: "https://github.com/maple",
	}}}}
	pattern := regexp.MustCompile(`MapleMono-NF-hinted\.zip`)
	selected, err := selectRelease(releases, ResolveRequest{
		CurrentVersion: "7.9", CurrentReleaseID: 79, CurrentAssetID: 7901,
	}, "subframe7536", "maple-font", "", pattern.String(), pattern)
	if err != nil {
		t.Fatal(err)
	}
	if !selected.Success || !selected.UpdateAvailable {
		t.Fatalf("selected %+v", selected)
	}
}

func TestSelectReleaseRejectsAmbiguousAndSidecarMatches(t *testing.T) {
	release := []apiRelease{{ID: 1, Tag: "v1.0.0", Assets: []apiAsset{
		{ID: 1, Name: "tool_1.0.0_amd64.deb"},
		{ID: 2, Name: "tool-1.0.0.x86_64.rpm"},
		{ID: 3, Name: "tool_1.0.0_amd64.deb.sig"},
		{ID: 4, Name: "manifest.json"},
	}}}
	pattern := regexp.MustCompile(`.*`)
	selected, err := selectRelease(release, ResolveRequest{}, "vendor", "tool", "",
		pattern.String(), pattern)
	if err != nil {
		t.Fatal(err)
	}
	if selected.Success || len(selected.AvailableAssets) != 4 || len(selected.MatchingAssets) != 2 ||
		!strings.Contains(selected.Message, "exactly one") {
		t.Fatalf("ambiguous selection %+v", selected)
	}

	sidecarPattern := regexp.MustCompile(`tool_1\.0\.0_amd64\.deb\.sig`)
	sidecar, err := selectRelease(release, ResolveRequest{}, "vendor", "tool", "",
		sidecarPattern.String(), sidecarPattern)
	if err != nil {
		t.Fatal(err)
	}
	if sidecar.Success || !strings.Contains(strings.ToLower(sidecar.Message), "sidecar") {
		t.Fatalf("sidecar selection %+v", sidecar)
	}
}

func TestSelectSourceArchive(t *testing.T) {
	releases := []apiRelease{{
		ID: 70, Tag: "v2.0.0-beta", Prerelease: true,
		TarballURL: "https://api.github.com/repos/vendor/tool/tarball/v2.0.0-beta",
		ZipballURL: "https://api.github.com/repos/vendor/tool/zipball/v2.0.0-beta",
	}}
	pattern := regexp.MustCompile(`Source code \(tar\.gz\)`)
	selected, err := selectRelease(releases, ResolveRequest{}, "vendor", "tool",
		"v2.0.0-beta", pattern.String(), pattern)
	if err != nil {
		t.Fatal(err)
	}
	if !selected.Success || selected.AssetID != 0 || selected.DetectedVersion != "2.0.0_beta" ||
		selected.Filename != "tool-2.0.0_beta.tar.gz" || len(selected.AvailableAssets) != 2 {
		t.Fatalf("source selection %+v", selected)
	}
}
