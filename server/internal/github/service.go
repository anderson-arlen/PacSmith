package github

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
)

const (
	apiVersion      = "2022-11-28"
	maxResponseSize = 8 << 20
	maxRegexLength  = 512
)

type Service struct {
	Secrets   *secret.LockedStore
	Artifacts *artifact.Registry
	Client    *http.Client
}

type ResolveRequest struct {
	URL                string `json:"url"`
	AssetRegex         string `json:"asset_regex"`
	IncludePrereleases bool   `json:"include_prereleases"`
	CurrentVersion     string `json:"current_version"`
	CurrentReleaseID   int64  `json:"current_release_id"`
	CurrentAssetID     int64  `json:"current_asset_id"`
	CurrentPrerelease  bool   `json:"current_prerelease"`
}

type ImportRequest struct {
	URL                string `json:"url"`
	AssetRegex         string `json:"asset_regex"`
	IncludePrereleases bool   `json:"include_prereleases"`
	ExistingProjectID  string `json:"existing_project_id"`
}

type Source struct {
	Success         bool     `json:"success"`
	UpdateAvailable bool     `json:"update_available"`
	DetectedVersion string   `json:"detected_version"`
	Filename        string   `json:"filename"`
	SHA256          string   `json:"sha256"`
	DownloadURL     string   `json:"download_url"`
	Message         string   `json:"message"`
	ReleaseID       int64    `json:"release_id"`
	AssetID         int64    `json:"asset_id"`
	Tag             string   `json:"tag"`
	PublisherDigest string   `json:"publisher_digest"`
	Prerelease      bool     `json:"prerelease"`
	Owner           string   `json:"owner"`
	Repository      string   `json:"repository"`
	AssetRegex      string   `json:"asset_regex"`
	RequestedTag    string   `json:"requested_tag"`
	AvailableAssets []string `json:"available_assets"`
	MatchingAssets  []string `json:"matching_assets"`
}

type apiRelease struct {
	ID         int64      `json:"id"`
	Tag        string     `json:"tag_name"`
	Draft      bool       `json:"draft"`
	Prerelease bool       `json:"prerelease"`
	TarballURL string     `json:"tarball_url"`
	ZipballURL string     `json:"zipball_url"`
	Assets     []apiAsset `json:"assets"`
}

type apiAsset struct {
	ID          int64  `json:"id"`
	Name        string `json:"name"`
	DownloadURL string `json:"browser_download_url"`
	Digest      string `json:"digest"`
	Filename    string `json:"-"`
}

func (s *Service) Resolve(ctx context.Context, request ResolveRequest) (Source, error) {
	owner, repository, requestedTag, impliedRegex, err := parseURL(request.URL)
	if err != nil {
		return Source{}, err
	}
	assetRegex := strings.TrimSpace(request.AssetRegex)
	if impliedRegex != "" {
		assetRegex = regexp.QuoteMeta(impliedRegex)
	}
	if assetRegex == "" {
		assetRegex = ".*"
	}
	if len(assetRegex) > maxRegexLength {
		return Source{}, fmt.Errorf("artifact regular expression exceeds %d characters", maxRegexLength)
	}
	pattern, err := regexp.Compile(assetRegex)
	if err != nil {
		return Source{}, fmt.Errorf("invalid artifact regular expression: %w", err)
	}

	endpoint := fmt.Sprintf("https://api.github.com/repos/%s/%s/releases?per_page=50",
		url.PathEscape(owner), url.PathEscape(repository))
	if requestedTag != "" {
		endpoint = fmt.Sprintf("https://api.github.com/repos/%s/%s/releases/tags/%s",
			url.PathEscape(owner), url.PathEscape(repository), url.PathEscape(requestedTag))
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return Source{}, err
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("X-GitHub-Api-Version", apiVersion)
	req.Header.Set("User-Agent", "PacSmith/0.2")
	if token, tokenErr := s.token(ctx); tokenErr != nil {
		return Source{}, tokenErr
	} else if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	response, err := s.httpClient().Do(req)
	if err != nil {
		return Source{}, fmt.Errorf("GitHub request: %w", err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, maxResponseSize+1))
	if err != nil {
		return Source{}, fmt.Errorf("read GitHub response: %w", err)
	}
	if len(body) > maxResponseSize {
		return Source{}, fmt.Errorf("GitHub response exceeds %d bytes", maxResponseSize)
	}
	if response.StatusCode != http.StatusOK {
		message := strings.TrimSpace(string(body))
		if len(message) > 2048 {
			message = message[:2048]
		}
		return Source{}, fmt.Errorf("GitHub request failed (HTTP %d)%s", response.StatusCode,
			rateLimitSuffix(response, message))
	}

	var releases []apiRelease
	if requestedTag != "" {
		var release apiRelease
		if err := json.Unmarshal(body, &release); err != nil {
			return Source{}, fmt.Errorf("decode GitHub release: %w", err)
		}
		releases = []apiRelease{release}
	} else if err := json.Unmarshal(body, &releases); err != nil {
		return Source{}, fmt.Errorf("decode GitHub releases: %w", err)
	}

	return selectRelease(releases, request, owner, repository, requestedTag, assetRegex, pattern)
}

func (s *Service) Download(ctx context.Context, source Source) (artifact.Record, error) {
	download, err := url.Parse(source.DownloadURL)
	if err != nil || download.Scheme != "https" || download.Host == "" || download.User != nil {
		return artifact.Record{}, fmt.Errorf("resolved GitHub artifact URL is not safe HTTPS")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, download.String(), nil)
	if err != nil {
		return artifact.Record{}, err
	}
	req.Header.Set("Accept", "application/octet-stream")
	req.Header.Set("User-Agent", "PacSmith/0.2")
	if token, tokenErr := s.token(ctx); tokenErr != nil {
		return artifact.Record{}, tokenErr
	} else if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	response, err := s.httpClient().Do(req)
	if err != nil {
		return artifact.Record{}, fmt.Errorf("download GitHub artifact: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return artifact.Record{}, fmt.Errorf("GitHub artifact download failed (HTTP %d)", response.StatusCode)
	}
	if response.ContentLength > artifact.MaxBytes {
		return artifact.Record{}, fmt.Errorf("GitHub artifact exceeds %d bytes", artifact.MaxBytes)
	}
	return s.Artifacts.Put(ctx, source.Filename, "vendor", &boundedReader{
		Reader: response.Body,
		N:      artifact.MaxBytes + 1,
	})
}

func (s *Service) token(ctx context.Context) (string, error) {
	if s.Secrets == nil {
		return "", nil
	}
	value, err := s.Secrets.Get(ctx, "github.token")
	if errors.Is(err, secret.ErrNotFound) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("read GitHub credential: %w", err)
	}
	return strings.TrimSpace(string(value)), nil
}

func (s *Service) httpClient() *http.Client {
	if s.Client != nil {
		return s.Client
	}
	return &http.Client{
		Timeout: 2 * time.Minute,
		CheckRedirect: func(req *http.Request, _ []*http.Request) error {
			if req.URL.Scheme != "https" {
				return fmt.Errorf("refusing non-HTTPS redirect")
			}
			return nil
		},
	}
}

type boundedReader struct {
	Reader io.Reader
	N      int64
}

func (r *boundedReader) Read(p []byte) (int, error) {
	if r.N <= 0 {
		return 0, fmt.Errorf("artifact exceeds %d bytes", artifact.MaxBytes)
	}
	if int64(len(p)) > r.N {
		p = p[:r.N]
	}
	n, err := r.Reader.Read(p)
	r.N -= int64(n)
	return n, err
}

func parseURL(raw string) (owner, repository, requestedTag, impliedRegex string, err error) {
	parsed, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || parsed.Scheme != "https" || parsed.User != nil || parsed.Fragment != "" ||
		(parsed.Hostname() != "github.com" && parsed.Hostname() != "www.github.com") ||
		(parsed.Port() != "" && parsed.Port() != "443") {
		return "", "", "", "", fmt.Errorf("enter an HTTPS github.com repository, release, or release-asset URL without credentials or a fragment")
	}
	parts := strings.FieldsFunc(parsed.EscapedPath(), func(r rune) bool { return r == '/' })
	for i := range parts {
		parts[i], err = url.PathUnescape(parts[i])
		if err != nil {
			return "", "", "", "", fmt.Errorf("invalid GitHub URL path")
		}
	}
	if len(parts) < 2 {
		return "", "", "", "", fmt.Errorf("GitHub URL must identify an owner and repository")
	}
	owner = parts[0]
	repository = strings.TrimSuffix(parts[1], ".git")
	if owner == "" || repository == "" {
		return "", "", "", "", fmt.Errorf("GitHub URL must identify an owner and repository")
	}
	if len(parts) >= 6 && parts[2] == "releases" && parts[3] == "download" {
		requestedTag = parts[4]
		impliedRegex = strings.Join(parts[5:], "/")
	} else if len(parts) >= 5 && parts[2] == "releases" && parts[3] == "tag" {
		requestedTag = strings.Join(parts[4:], "/")
	}
	return owner, repository, requestedTag, impliedRegex, nil
}

func selectRelease(releases []apiRelease, request ResolveRequest, owner, repository,
	requestedTag, assetRegex string, pattern *regexp.Regexp) (Source, error) {
	ordered := make([]apiRelease, 0, len(releases))
	appendMatching := func(prerelease *bool) {
		for _, release := range releases {
			if release.Draft || (requestedTag != "" && release.Tag != requestedTag) {
				continue
			}
			if prerelease != nil && release.Prerelease != *prerelease {
				continue
			}
			ordered = append(ordered, release)
		}
	}
	if requestedTag != "" || request.IncludePrereleases {
		appendMatching(nil)
	} else {
		stable, preview := false, true
		appendMatching(&stable)
		appendMatching(&preview)
	}

	result := Source{Success: true, Owner: owner, Repository: repository,
		AssetRegex: assetRegex, RequestedTag: requestedTag, AvailableAssets: []string{},
		MatchingAssets: []string{}}
	for _, release := range ordered {
		assets := append([]apiAsset(nil), release.Assets...)
		if release.TarballURL != "" {
			assets = append(assets, apiAsset{Name: "Source code (tar.gz)", Filename: sourceFilename(repository, release.Tag, "tar.gz"), DownloadURL: release.TarballURL})
		}
		if release.ZipballURL != "" {
			assets = append(assets, apiAsset{Name: "Source code (zip)", Filename: sourceFilename(repository, release.Tag, "zip"), DownloadURL: release.ZipballURL})
		}
		result.AvailableAssets = result.AvailableAssets[:0]
		result.MatchingAssets = result.MatchingAssets[:0]
		var matches []apiAsset
		var sidecars []string
		for _, asset := range assets {
			result.AvailableAssets = append(result.AvailableAssets, asset.Name)
			location := pattern.FindStringIndex(asset.Name)
			if location == nil || location[0] != 0 || location[1] != len(asset.Name) {
				continue
			}
			if isSidecar(asset.Name) {
				sidecars = append(sidecars, asset.Name)
				continue
			}
			matches = append(matches, asset)
			result.MatchingAssets = append(result.MatchingAssets, asset.Name)
		}
		if len(matches) == 0 {
			if len(sidecars) > 0 {
				result.Success = false
				result.Message = "The GitHub asset rule selected verification sidecars, not an installable package artifact"
				return result, nil
			}
			continue
		}
		if len(matches) != 1 {
			result.Success = false
			result.Message = fmt.Sprintf("Release %s has %d artifacts matching /%s/; exactly one is required",
				release.Tag, len(matches), assetRegex)
			return result, nil
		}
		asset := matches[0]
		result.ReleaseID = release.ID
		result.AssetID = asset.ID
		result.Tag = release.Tag
		result.DetectedVersion = versionFromTag(release.Tag)
		result.Filename = asset.Filename
		if result.Filename == "" {
			result.Filename = asset.Name
		}
		result.DownloadURL = asset.DownloadURL
		result.PublisherDigest = asset.Digest
		if strings.HasPrefix(strings.ToLower(asset.Digest), "sha256:") {
			result.SHA256 = strings.ToLower(strings.TrimPrefix(strings.ToLower(asset.Digest), "sha256:"))
		}
		result.Prerelease = release.Prerelease
		available, err := newerVersion(result.DetectedVersion, request.CurrentVersion)
		if err != nil {
			return Source{}, err
		}
		if request.CurrentReleaseID != 0 && request.CurrentReleaseID == release.ID {
			if request.CurrentAssetID == 0 || request.CurrentAssetID == asset.ID {
				available = false
			} else {
				available = true
			}
		}
		result.UpdateAvailable = available
		if !release.Prerelease && request.CurrentPrerelease && request.CurrentReleaseID != 0 &&
			request.CurrentReleaseID != release.ID && numericVersionCore(request.CurrentVersion) != "" &&
			numericVersionCore(request.CurrentVersion) == numericVersionCore(result.DetectedVersion) {
			result.UpdateAvailable = true
		}
		fallback := release.Prerelease && requestedTag == "" && !request.IncludePrereleases
		if result.UpdateAvailable {
			result.Message = fmt.Sprintf("GitHub release %s is available (%s)", result.Tag, result.Filename)
			if fallback {
				result.Message = fmt.Sprintf("No matching stable release is available; GitHub prerelease %s is available (%s)", result.Tag, result.Filename)
			}
		} else {
			result.Message = fmt.Sprintf("GitHub release %s is current", result.Tag)
			if fallback {
				result.Message = fmt.Sprintf("No matching stable release is available; GitHub prerelease %s is current", result.Tag)
			}
		}
		return result, nil
	}
	result.Message = fmt.Sprintf("No published GitHub release has an artifact matching /%s/", assetRegex)
	return result, nil
}

func newerVersion(candidate, current string) (bool, error) {
	current = strings.TrimSpace(current)
	if current == "" || current == "0" {
		return true, nil
	}
	out, err := exec.Command("/usr/bin/vercmp", candidate, current).Output()
	if err != nil {
		return false, fmt.Errorf("compare GitHub release versions: %w", err)
	}
	comparison, err := strconv.Atoi(strings.TrimSpace(string(out)))
	if err != nil {
		return false, fmt.Errorf("parse vercmp output %q: %w", strings.TrimSpace(string(out)), err)
	}
	return comparison > 0, nil
}

func versionFromTag(tag string) string {
	tag = strings.TrimSpace(tag)
	if len(tag) > 1 && (tag[0] == 'v' || tag[0] == 'V') && tag[1] >= '0' && tag[1] <= '9' {
		tag = tag[1:]
	}
	normalized := regexp.MustCompile(`[^A-Za-z0-9+._]+`).ReplaceAllString(tag, "_")
	normalized = regexp.MustCompile(`_+`).ReplaceAllString(normalized, "_")
	normalized = strings.Trim(normalized, "_")
	if normalized == "" {
		return "0"
	}
	return normalized
}

func sourceFilename(repository, tag, extension string) string {
	repository = regexp.MustCompile(`[^A-Za-z0-9._+-]+`).ReplaceAllString(strings.TrimSpace(repository), "-")
	repository = strings.Trim(repository, "-")
	if repository == "" {
		repository = "source"
	}
	return repository + "-" + versionFromTag(tag) + "." + extension
}

func isSidecar(name string) bool {
	lower := strings.ToLower(name)
	return strings.HasSuffix(lower, ".sig") || strings.HasSuffix(lower, ".asc") ||
		strings.HasSuffix(lower, ".sha256") || strings.HasSuffix(lower, ".sha512") ||
		strings.Contains(lower, "checksums") || strings.Contains(lower, "sha256sums") ||
		lower == "manifest.json"
}

func numericVersionCore(value string) string {
	return regexp.MustCompile(`[0-9]+(?:\.[0-9]+)+`).FindString(value)
}

func rateLimitSuffix(response *http.Response, message string) string {
	parts := []string{}
	if remaining := response.Header.Get("X-RateLimit-Remaining"); remaining != "" {
		parts = append(parts, "GitHub rate limit remaining: "+remaining)
	}
	if reset := response.Header.Get("X-RateLimit-Reset"); reset != "" {
		parts = append(parts, "reset epoch "+reset)
	}
	if message != "" {
		parts = append(parts, message)
	}
	if len(parts) == 0 {
		return ""
	}
	return ": " + strings.Join(parts, ", ")
}
