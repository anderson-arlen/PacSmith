package updatecheck

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"regexp"
	"sort"
	"strings"
	"time"
)

type PPARequest struct {
	PPA          string `json:"ppa"`
	Architecture string `json:"architecture"`
}

type PPAResolution struct {
	PPA                string              `json:"ppa"`
	DisplayName        string              `json:"display_name"`
	RepositoryURL      string              `json:"repository_url"`
	SigningKeyURL      string              `json:"signing_key_url"`
	SigningFingerprint string              `json:"signing_fingerprint"`
	Architecture       string              `json:"architecture"`
	Packages           map[string][]string `json:"packages"`
}

var ppaNamePattern = regexp.MustCompile(`^[a-z0-9][a-z0-9+.-]*$`)
var ppaFingerprintPattern = regexp.MustCompile(`^[A-F0-9]{40}$`)
var ppaPackagePattern = regexp.MustCompile(`^[a-z0-9][a-z0-9+.-]+$`)

// Launchpad's package listing is discovery evidence. Import still verifies the
// selected package against the signed APT indexes using the reviewed key.
func (s *Service) ResolvePPA(ctx context.Context, request PPARequest) (PPAResolution, error) {
	parts := strings.Split(strings.TrimPrefix(strings.TrimSpace(request.PPA), "ppa:"), "/")
	if len(parts) != 2 || !ppaNamePattern.MatchString(parts[0]) || !ppaNamePattern.MatchString(parts[1]) {
		return PPAResolution{}, fmt.Errorf("enter a PPA as ppa:owner/archive")
	}
	architecture := strings.TrimSpace(request.Architecture)
	if architecture == "" {
		architecture = "amd64"
	}
	if !ppaNamePattern.MatchString(architecture) {
		return PPAResolution{}, fmt.Errorf("invalid Debian architecture")
	}
	ctx, cancel := context.WithTimeout(ctx, 90*time.Second)
	defer cancel()
	archiveURL := "https://api.launchpad.net/1.0/~" + parts[0] + "/+archive/ubuntu/" + parts[1]
	var archive struct {
		DisplayName  string `json:"displayname"`
		Private      bool   `json:"private"`
		Publish      bool   `json:"publish"`
		Distribution string `json:"distribution_link"`
		Fingerprint  string `json:"signing_key_fingerprint"`
	}
	if err := s.ppaJSON(ctx, archiveURL, &archive); err != nil {
		return PPAResolution{}, err
	}
	if archive.Private || !archive.Publish || archive.Distribution != "https://api.launchpad.net/1.0/ubuntu" {
		return PPAResolution{}, fmt.Errorf("PPA must be a public, publishing Ubuntu archive")
	}
	fingerprint := strings.ToUpper(strings.TrimSpace(archive.Fingerprint))
	if !ppaFingerprintPattern.MatchString(fingerprint) {
		return PPAResolution{}, fmt.Errorf("Launchpad did not provide a usable PPA signing-key fingerprint")
	}
	result := PPAResolution{
		PPA: "ppa:" + strings.Join(parts, "/"), DisplayName: archive.DisplayName,
		RepositoryURL:      "https://ppa.launchpadcontent.net/" + strings.Join(parts, "/") + "/ubuntu",
		SigningKeyURL:      "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x" + fingerprint,
		SigningFingerprint: fingerprint, Architecture: architecture,
		Packages: make(map[string][]string),
	}
	next := archiveURL + "?ws.op=getPublishedBinaries&status=Published&ws.size=300"
	seenPages := make(map[string]bool)
	packages := make(map[string]map[string]bool)
	for page := 0; next != ""; page++ {
		if page >= 100 || seenPages[next] {
			return PPAResolution{}, fmt.Errorf("Launchpad package listing exceeds the page limit or repeats a page")
		}
		seenPages[next] = true
		parsed, err := url.Parse(next)
		if err != nil || parsed.Scheme != "https" || parsed.Host != "api.launchpad.net" ||
			parsed.User != nil || parsed.Fragment != "" || parsed.Path != strings.TrimPrefix(archiveURL, "https://api.launchpad.net") {
			return PPAResolution{}, fmt.Errorf("Launchpad returned an unsafe package-list URL")
		}
		var listing struct {
			Next    string `json:"next_collection_link"`
			Entries []struct {
				Name       string `json:"binary_package_name"`
				ArchSeries string `json:"distro_arch_series_link"`
				Status     string `json:"status"`
			} `json:"entries"`
		}
		if err := s.ppaJSON(ctx, next, &listing); err != nil {
			return PPAResolution{}, err
		}
		for _, entry := range listing.Entries {
			if !strings.HasPrefix(entry.ArchSeries, "https://api.launchpad.net/1.0/ubuntu/") {
				continue
			}
			series := strings.Split(strings.TrimPrefix(entry.ArchSeries, "https://api.launchpad.net/1.0/ubuntu/"), "/")
			if len(series) != 2 || series[1] != architecture || !ppaNamePattern.MatchString(series[0]) ||
				entry.Status != "Published" || !ppaPackagePattern.MatchString(entry.Name) {
				continue
			}
			if packages[series[0]] == nil {
				packages[series[0]] = make(map[string]bool)
			}
			packages[series[0]][entry.Name] = true
		}
		next = listing.Next
	}
	for suite, names := range packages {
		for name := range names {
			result.Packages[suite] = append(result.Packages[suite], name)
		}
		sort.Strings(result.Packages[suite])
	}
	if len(result.Packages) == 0 {
		return PPAResolution{}, fmt.Errorf("PPA has no published packages for %s", architecture)
	}
	return result, nil
}

func (s *Service) ppaJSON(ctx context.Context, rawURL string, destination any) error {
	target, err := url.Parse(rawURL)
	if err != nil {
		return err
	}
	body, _, status, err := s.request(ctx, http.MethodGet, target, nil, 4<<20)
	if err != nil || !successfulStatus(status) {
		return requestFailure("Launchpad PPA lookup failed", status, err)
	}
	if err := json.Unmarshal(body, destination); err != nil {
		return fmt.Errorf("invalid Launchpad PPA response: %w", err)
	}
	return nil
}
