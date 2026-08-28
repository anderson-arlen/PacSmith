package updatecheck

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path/filepath"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/pgp"
)

type DirectImportRequest struct {
	URL               string `json:"url"`
	ExistingProjectID string `json:"existing_project_id"`
	Version           string `json:"version"`
	ExpectedSHA256    string `json:"expected_sha256"`
}

type RepositoryImportRequest struct {
	Update            map[string]any `json:"update"`
	TrustedSigningKey string         `json:"trusted_signing_key"`
	SigningKeySource  string         `json:"signing_key_source"`
	PinnedFingerprint string         `json:"pinned_fingerprint"`
}

type RepositoryKeyInspection struct {
	Contents     string   `json:"contents"`
	RequestedURL string   `json:"requested_url"`
	ResolvedURL  string   `json:"resolved_url"`
	SHA256       string   `json:"sha256"`
	Fingerprints []string `json:"fingerprints"`
}

func (s *Service) InspectRepositoryKey(ctx context.Context, rawURL string) (RepositoryKeyInspection, error) {
	requested, err := url.Parse(strings.TrimSpace(rawURL))
	if err != nil || validateHTTPURL(requested) != nil || requested.Scheme != "https" || requested.Fragment != "" {
		return RepositoryKeyInspection{}, fmt.Errorf("signing-key URL must be HTTPS without credentials or a fragment")
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, requested.String(), nil)
	if err != nil {
		return RepositoryKeyInspection{}, err
	}
	request.Header.Set("User-Agent", userAgent)
	request.Header.Set("Accept-Encoding", "identity")
	response, err := s.httpClient().Do(request)
	if err != nil {
		return RepositoryKeyInspection{}, err
	}
	defer response.Body.Close()
	if !successfulStatus(response.StatusCode) {
		return RepositoryKeyInspection{}, fmt.Errorf("signing-key download failed (HTTP %d)", response.StatusCode)
	}
	resolved := requested
	if response.Request != nil && response.Request.URL != nil {
		resolved = response.Request.URL
	}
	if validateHTTPURL(resolved) != nil || resolved.Scheme != "https" || resolved.Fragment != "" {
		return RepositoryKeyInspection{}, fmt.Errorf("signing-key download resolved to an unacceptable URL")
	}
	const maximumKeyBytes = 4 << 20
	contents, err := io.ReadAll(io.LimitReader(response.Body, maximumKeyBytes+1))
	if err != nil {
		return RepositoryKeyInspection{}, err
	}
	if len(contents) == 0 {
		return RepositoryKeyInspection{}, fmt.Errorf("signing-key URL returned an empty response")
	}
	if len(contents) > maximumKeyBytes {
		return RepositoryKeyInspection{}, fmt.Errorf("signing key exceeds the 4 MiB safety limit")
	}
	normalized, err := pgp.Normalize(contents)
	if err != nil {
		return RepositoryKeyInspection{}, err
	}
	fingerprints, err := pgp.Fingerprints(normalized)
	if err != nil || len(fingerprints) == 0 {
		return RepositoryKeyInspection{}, fmt.Errorf("downloaded file is not an OpenPGP public key")
	}
	digest := sha256.Sum256(normalized)
	return RepositoryKeyInspection{
		Contents: base64.StdEncoding.EncodeToString(normalized), RequestedURL: requested.String(),
		ResolvedURL: resolved.String(), SHA256: hex.EncodeToString(digest[:]), Fingerprints: fingerprints,
	}, nil
}

func (s *Service) ImportDirectURL(ctx context.Context, request DirectImportRequest, jobID string,
	log func(string), reporters ...func(Progress)) (result library.ImportResult, resultErr error) {
	if log == nil {
		log = func(string) {}
	}
	report := progressReporter(reporters)
	parsed, err := url.Parse(strings.TrimSpace(request.URL))
	if err != nil || validateHTTPURL(parsed) != nil || parsed.Scheme != "https" || parsed.Fragment != "" {
		return library.ImportResult{}, fmt.Errorf("Direct artifact URL must be HTTPS without credentials or a fragment")
	}
	expectedSHA256 := strings.ToLower(strings.TrimSpace(request.ExpectedSHA256))
	if expectedSHA256 != "" && !sha256Pattern.MatchString(expectedSHA256) {
		return library.ImportResult{}, fmt.Errorf("expected SHA256 must contain 64 hexadecimal characters")
	}
	canonical := *parsed
	canonical.RawQuery = ""
	canonical.ForceQuery = false
	acquisition, _ := json.Marshal(map[string]any{
		"kind": "direct-url", "canonicalIdentity": canonical.String(),
		"originalUrl": parsed.String(), "publisherDigest": expectedSHA256,
		"publisherVerified": expectedSHA256 != "",
	})
	started := library.ImportResult{}
	projectName := ""
	packageName := ""
	defer func() {
		if resultErr == nil || started.ReleaseID == "" {
			return
		}
		status := "failed"
		if errors.Is(resultErr, context.Canceled) {
			status = "canceled"
		}
		_ = s.Library.FinishPendingImport(context.Background(), started.ReleaseID,
			status, resultErr.Error())
	}()
	log("Downloading vendor artifact…\n")
	record, err := s.downloadArtifact(ctx, parsed, filepath.Base(parsed.Path), "vendor",
		downloadObserver{
			Started: func(info downloadInfo) error {
				started, err = s.Library.BeginPendingImport(ctx, library.PendingImportRequest{
					JobID: jobID, ExistingProjectID: request.ExistingProjectID,
					Version: request.Version, Filename: info.Filename,
					CanonicalIdentity: canonical.String(), SourceURL: parsed.String(),
					Acquisition: acquisition, ContentLength: info.ContentLength,
					Received: info.Received,
				})
				if err != nil {
					return err
				}
				if project, projectErr := s.Library.GetProject(ctx, started.ProjectID); projectErr == nil {
					projectName = project.DisplayName
					packageName = project.ArchPackageName
				}
				report(Progress{Message: "Downloading " + info.Filename,
					ProjectID: started.ProjectID, ReleaseID: started.ReleaseID,
					ProjectName: projectName, PackageName: packageName,
					Current: info.Received, Total: info.ContentLength})
				return nil
			},
			Progress: func(info downloadInfo) {
				report(Progress{Message: "Downloading " + info.Filename,
					ProjectID: started.ProjectID, ReleaseID: started.ReleaseID,
					ProjectName: projectName, PackageName: packageName,
					Current: info.Received, Total: info.ContentLength})
			},
		})
	if err != nil {
		return library.ImportResult{}, err
	}
	if expectedSHA256 != "" && expectedSHA256 != record.SHA256 {
		return library.ImportResult{}, fmt.Errorf("downloaded artifact SHA256 does not match the expected digest")
	}
	log("Inspecting vendor artifact…\n")
	report(Progress{Message: "Inspecting vendor artifact", ProjectID: started.ProjectID,
		ReleaseID: started.ReleaseID, ProjectName: projectName, PackageName: packageName,
		Current: record.SizeBytes, Total: record.SizeBytes})
	return s.Library.ImportArtifact(ctx, library.ImportRequest{
		ArtifactID: record.ID, ExistingProjectID: started.ProjectID,
		Version: request.Version, ExpectedSHA256: expectedSHA256,
		AcquisitionKind: "direct-url", CanonicalIdentity: canonical.String(),
		Acquisition: acquisition, PendingReleaseID: started.ReleaseID,
		PendingProjectCreated: started.ProjectCreated,
	})
}

func (s *Service) ImportRepository(ctx context.Context, request RepositoryImportRequest,
	log func(string)) (library.ImportResult, error) {
	if log == nil {
		log = func(string) {}
	}
	update := cloneObject(request.Update)
	strategy := stringValue(update, "strategy")
	if strategy != StrategyAPT && strategy != StrategyRPM {
		return library.ImportResult{}, fmt.Errorf("repository import strategy must be APT or RPM")
	}
	keyContents, err := base64.StdEncoding.DecodeString(strings.TrimSpace(request.TrustedSigningKey))
	if err != nil {
		return library.ImportResult{}, fmt.Errorf("decode repository signing key: %w", err)
	}
	normalized, err := pgp.Normalize(keyContents)
	if err != nil {
		return library.ImportResult{}, err
	}
	fingerprints, err := pgp.Fingerprints(normalized)
	if err != nil {
		return library.ImportResult{}, err
	}
	if len(fingerprints) == 0 {
		return library.ImportResult{}, fmt.Errorf("repository signing key contains no fingerprints")
	}
	pinned := strings.ToUpper(strings.TrimSpace(request.PinnedFingerprint))
	if pinned == "" {
		pinned = fingerprints[0]
	}
	found := false
	for _, fingerprint := range fingerprints {
		if strings.EqualFold(fingerprint, pinned) {
			found = true
			break
		}
	}
	if !found {
		return library.ImportResult{}, fmt.Errorf("repository signing key does not contain the pinned fingerprint")
	}
	keyRecord, err := s.Artifacts.Put(ctx, "repository-signing-key.gpg", "signing_key",
		bytes.NewReader(normalized))
	if err != nil {
		return library.ImportResult{}, err
	}
	keyPath := "files/keys/vendor-" + keyRecord.SHA256[:16] + ".gpg"
	fingerprintValues := make([]any, 0, len(fingerprints))
	for _, fingerprint := range fingerprints {
		fingerprintValues = append(fingerprintValues, fingerprint)
	}
	update["aptSigningKeyring"] = keyPath
	update["trustedSigningFingerprint"] = pinned
	update["signingKeys"] = []any{map[string]any{
		"relativePath": keyPath, "sha256": keyRecord.SHA256,
		"fingerprints": fingerprintValues, "trusted": true, "artifactId": keyRecord.ID,
		"sourcePath": request.SigningKeySource,
	}}
	packageName := stringValue(update, "aptPackageName")
	architecture := stringValue(update, "aptArchitecture")
	if strategy == StrategyRPM {
		packageName = stringValue(update, "rpmPackageName")
		architecture = stringValue(update, "rpmArchitecture")
	}
	target := checkTarget{
		Release: library.Release{Document: map[string]any{
			"originalSourceFilename": packageName,
			"debian":                 map[string]any{"package": packageName, "version": "0", "architecture": architecture},
		}},
		Update: update, Version: "0",
	}
	var checked Result
	if strategy == StrategyRPM {
		checked, err = s.checkRPM(ctx, target, log)
	} else {
		checked, err = s.checkAPT(ctx, target, log)
	}
	if err != nil {
		return library.ImportResult{}, err
	}
	downloadURL, err := url.Parse(checked.DownloadURL)
	if err != nil {
		return library.ImportResult{}, err
	}
	record := checked.Artifact
	if record == nil {
		log("Downloading signed repository package…\n")
		downloaded, downloadErr := s.downloadArtifact(ctx, downloadURL, checked.Filename, "vendor")
		if downloadErr != nil {
			return library.ImportResult{}, downloadErr
		}
		record = &downloaded
	}
	if !strings.EqualFold(record.SHA256, checked.SHA256) {
		return library.ImportResult{}, fmt.Errorf("repository package SHA256 does not match signed metadata")
	}
	update["detectedVersion"] = checked.DetectedVersion
	update["detectedFilename"] = checked.Filename
	update["detectedSha256"] = checked.SHA256
	update["detectedUrl"] = checked.DownloadURL
	update["lastChecked"] = timestamp(s.now())
	update["lastCheckMessage"] = checked.Message
	update["lastCheckFailed"] = false
	update["signatureVerified"] = true
	rawUpdate, _ := json.Marshal(update)
	kind := "apt-repository"
	canonical := fmt.Sprintf("apt:%s:%s:%s:%s:%s", strings.ToLower(stringValue(update, "url")),
		strings.ToLower(stringValue(update, "aptSuite")), strings.ToLower(stringValue(update, "aptComponent")),
		strings.ToLower(architecture), strings.ToLower(packageName))
	if strategy == StrategyRPM {
		kind = "rpm-repository"
		canonical = fmt.Sprintf("rpm:%s:%s:%s", strings.ToLower(stringValue(update, "url")),
			strings.ToLower(architecture), strings.ToLower(packageName))
	}
	acquisition, _ := json.Marshal(map[string]any{
		"kind": kind, "canonicalIdentity": canonical, "originalUrl": checked.DownloadURL,
		"publisherDigest": checked.SHA256, "publisherVerified": true,
	})
	return s.Library.ImportArtifact(ctx, library.ImportRequest{
		ArtifactID: record.ID, Version: checked.DetectedVersion, ExpectedSHA256: checked.SHA256,
		AcquisitionKind: kind, CanonicalIdentity: canonical, Acquisition: acquisition,
		Update: rawUpdate, TrustedSigningKey: base64.StdEncoding.EncodeToString(normalized),
		TrustedSigningKeySource: request.SigningKeySource,
	})
}
