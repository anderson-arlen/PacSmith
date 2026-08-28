package updatecheck

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	githubapi "github.com/anderson-arlen/pacsmith/server/internal/github"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

type Service struct {
	DB                  *sqlite.DB
	Library             *library.Service
	Artifacts           *artifact.Registry
	GitHub              *githubapi.Service
	Client              *http.Client
	Now                 func() time.Time
	DownloadIdleTimeout time.Duration
}

func (s *Service) downloadIdleTimeout() time.Duration {
	if s.DownloadIdleTimeout > 0 {
		return s.DownloadIdleTimeout
	}
	return 45 * time.Second
}

func (s *Service) httpClient() *http.Client {
	if s.Client != nil {
		return s.Client
	}
	return defaultHTTPClient()
}

func (s *Service) now() time.Time {
	if s.Now != nil {
		return s.Now()
	}
	return time.Now()
}

func (s *Service) Run(ctx context.Context, releaseID string, force bool, log func(string),
	reporters ...func(Progress)) (BatchResult, error) {
	if log == nil {
		log = func(string) {}
	}
	report := progressReporter(reporters)
	if releaseID != "" {
		release, err := s.Library.GetRelease(ctx, releaseID)
		if err != nil {
			return BatchResult{}, err
		}
		project, err := s.Library.GetProject(ctx, release.ProjectID)
		if err != nil {
			return BatchResult{}, err
		}
		target := checkTargetFrom(project, release)
		targetLog := progressLogger(log, report, target, 1, 1)
		targetLog(fmt.Sprintf("Checking %s for updates…\n", project.DisplayName))
		result := s.check(ctx, target, force, targetLog)
		result = s.reconcilePreparedBuild(ctx, project.ID, result, targetLog)
		reportResult(report, target, result, 1, 1)
		batch := BatchResult{Checks: []Result{result}}
		if result.Status == "error" {
			batch.Failed = 1
		}
		return batch, nil
	}
	projects, err := s.Library.ListProjects(ctx)
	if err != nil {
		return BatchResult{}, err
	}
	batch := BatchResult{Checks: make([]Result, 0, len(projects))}
	for index, project := range projects {
		if err := ctx.Err(); err != nil {
			return batch, err
		}
		release := activeTrackingRelease(project)
		if release == nil {
			result := Result{ProjectID: project.ID, ProjectName: project.DisplayName,
				PackageName: project.ArchPackageName, Status: "paused",
				Message: "project has no analyzed release to track"}
			batch.Checks = append(batch.Checks, result)
			report(Progress{Message: result.Message, ProjectID: project.ID,
				ProjectName: project.DisplayName, PackageName: project.ArchPackageName,
				Current: int64(index + 1), Total: int64(len(projects))})
			continue
		}
		target := checkTargetFrom(project, *release)
		targetLog := progressLogger(log, report, target, index+1, len(projects))
		targetLog(fmt.Sprintf("Checking %s for updates…\n", project.DisplayName))
		result := s.check(ctx, target, force, targetLog)
		result = s.reconcilePreparedBuild(ctx, project.ID, result, targetLog)
		reportResult(report, target, result, index+1, len(projects))
		batch.Checks = append(batch.Checks, result)
		if result.Status == "error" {
			batch.Failed++
		}
	}
	return batch, nil
}

func (s *Service) reconcilePreparedBuild(ctx context.Context, projectID string, result Result,
	log func(string)) Result {
	if result.Status != "no-update" || result.UpdateAvailable {
		return result
	}
	settings, err := s.DB.Queries.GetLibrarySettings(ctx)
	if err != nil || settings.UpdatesAutoPrepare == 0 {
		return result
	}
	project, err := s.Library.GetProject(ctx, projectID)
	if err != nil {
		return result
	}
	var prepared *library.Release
	var previous *library.Release
	for index := range project.Releases {
		candidate := &project.Releases[index]
		built := stringValue(candidate.Document, "buildStatus") == "succeeded" ||
			len(stringValues(candidate.Document["builtArtifactIds"])) > 0
		if built {
			if previous == nil || releaseVersionCompare(*candidate, *previous) > 0 {
				previous = candidate
			}
			continue
		}
		if candidate.State != "ready" && candidate.State != "needs-review" {
			continue
		}
		if prepared == nil || releaseVersionCompare(*candidate, *prepared) > 0 {
			prepared = candidate
		}
	}
	if prepared == nil || previous != nil && releaseVersionCompare(*prepared, *previous) <= 0 {
		return result
	}
	result.Prepared = true
	result.DiscoveredReleaseID = prepared.ID
	if err := s.markPreparedReleaseState(ctx, prepared.ID); err != nil {
		result.AutomaticStatus = "paused"
		result.AutomaticMessage = "could not determine whether the prepared release is ready: " + err.Error()
	} else if project.AutoBuildPolicy == "never" {
		result.AutomaticStatus = "build-disabled"
		result.AutomaticMessage = "this project's auto-build policy is Never"
	} else if previous == nil {
		result.AutomaticStatus = "paused"
		result.AutomaticMessage = "previous package configuration has no successful build"
	} else {
		target := checkTargetFrom(project, *previous)
		if built, buildErr := s.buildIfReviewFree(ctx, target, prepared.ID, log); buildErr != nil {
			result.AutomaticStatus = "paused"
			result.AutomaticMessage = buildErr.Error()
			log("Automatic build paused: " + buildErr.Error() + "\n")
		} else {
			result.Built = built
			setAutomaticBuildOutcome(&result, built)
		}
	}
	if persistErr := s.persistAutomaticOutcome(ctx, prepared.ID, result); persistErr != nil {
		log("Could not save automatic handling outcome: " + persistErr.Error() + "\n")
	}
	return result
}

func (s *Service) PrepareDiscovered(ctx context.Context, releaseID string,
	log func(string), reporters ...func(Progress)) (library.ImportResult, error) {
	release, err := s.Library.GetRelease(ctx, releaseID)
	if err != nil {
		return library.ImportResult{}, err
	}
	if release.State != "discovered" {
		return library.ImportResult{}, fmt.Errorf("release is not awaiting preparation")
	}
	project, err := s.Library.GetProject(ctx, release.ProjectID)
	if err != nil {
		return library.ImportResult{}, err
	}
	target := checkTargetFrom(project, release)
	log = progressLogger(log, progressReporter(reporters), target, 1, 1)
	acquisition := object(release.Document["acquisition"])
	result := Result{
		DetectedVersion:   release.VendorVersion,
		Filename:          stringValue(release.Document, "originalSourceFilename"),
		SHA256:            release.SourceSHA256,
		DownloadURL:       stringValue(release.Document, "sourceUrl"),
		ProviderReleaseID: int64Value(acquisition, "githubReleaseId"),
		ProviderAssetID:   int64Value(acquisition, "githubAssetId"),
		ProviderTag:       stringValue(acquisition, "githubTag"),
		PublisherDigest:   stringValue(acquisition, "publisherDigest"),
		Prerelease:        boolValue(acquisition, "githubPrerelease"),
	}
	return s.prepare(ctx, target, result, log)
}

func progressReporter(reporters []func(Progress)) func(Progress) {
	if len(reporters) > 0 && reporters[0] != nil {
		return reporters[0]
	}
	return func(Progress) {}
}

func progressLogger(log func(string), report func(Progress), target checkTarget,
	current, total int) func(string) {
	if log == nil {
		log = func(string) {}
	}
	return func(message string) {
		log(message)
		trimmed := strings.TrimSpace(message)
		if trimmed == "" {
			return
		}
		report(Progress{Message: trimmed, ProjectID: target.Project.ID,
			ReleaseID: target.Release.ID, ProjectName: target.Project.DisplayName,
			PackageName: target.Project.ArchPackageName, Current: int64(current), Total: int64(total)})
	}
}

func reportResult(report func(Progress), target checkTarget, result Result, current, total int) {
	report(Progress{Message: result.Message, ProjectID: target.Project.ID,
		ReleaseID: target.Release.ID, ProjectName: target.Project.DisplayName,
		PackageName: target.Project.ArchPackageName, Current: int64(current), Total: int64(total)})
}

func checkTargetFrom(project library.Project, release library.Release) checkTarget {
	debian := object(release.Document["debian"])
	return checkTarget{Project: project, Release: release, Update: object(release.Document["update"]),
		Version: stringValue(debian, "version")}
}

func (s *Service) check(ctx context.Context, target checkTarget, force bool, log func(string)) Result {
	result := Result{ProjectID: target.Project.ID, ReleaseID: target.Release.ID,
		ProjectName: target.Project.DisplayName, PackageName: target.Project.ArchPackageName}
	if target.Update == nil {
		result.Status = "paused"
		result.Message = "release has no update configuration"
		return result
	}
	var checked Result
	var err error
	switch stringValue(target.Update, "strategy") {
	case StrategyDirect:
		checked, err = s.checkDirect(ctx, target, force, log)
	case StrategyAPT:
		checked, err = s.checkAPT(ctx, target, log)
	case StrategyRPM:
		checked, err = s.checkRPM(ctx, target, log)
	case StrategyGitHub:
		checked, err = s.checkGitHub(ctx, target, log)
	default:
		checked.Status = "paused"
		checked.Message = "manual update source; no automatic check performed"
	}
	checked.ProjectID = target.Project.ID
	checked.ReleaseID = target.Release.ID
	checked.ProjectName = target.Project.DisplayName
	checked.PackageName = target.Project.ArchPackageName
	if err != nil {
		checked.Status = "error"
		checked.Message = err.Error()
	}
	if persistErr := s.persistObservation(ctx, target, checked); persistErr != nil {
		if checked.Status != "error" {
			checked.Status = "error"
			checked.Message = "save update check: " + persistErr.Error()
		} else {
			log("Could not save failed update observation: " + persistErr.Error() + "\n")
		}
		return checked
	}
	if checked.Status != "update" || !checked.UpdateAvailable {
		return checked
	}
	refreshed, refreshErr := s.Library.GetRelease(ctx, target.Release.ID)
	if refreshErr != nil {
		checked.Status = "error"
		checked.Message = "reload successful update observation: " + refreshErr.Error()
		return checked
	}
	target.Release = refreshed
	target.Update = object(refreshed.Document["update"])
	discovered, discoveryErr := s.recordDiscovery(ctx, target, checked)
	if discoveryErr != nil {
		checked.Status = "error"
		checked.Message = "record discovered update: " + discoveryErr.Error()
		return checked
	}
	checked.DiscoveredReleaseID = discovered.ID
	checked.AutomaticStatus = "discovered"
	settings, settingsErr := s.DB.Queries.GetLibrarySettings(ctx)
	if settingsErr != nil {
		checked.Status = "error"
		checked.Message += "; read automatic update settings: " + settingsErr.Error()
		return checked
	}
	if settings.UpdatesAutoPrepare == 0 {
		checked.AutomaticStatus = "disabled"
		checked.AutomaticMessage = "automatic download and preparation is disabled in library settings"
		return checked
	}
	imported, prepareErr := s.prepare(ctx, target, checked, log)
	if prepareErr != nil {
		checked.Status = "error"
		checked.Message += "; automatic preparation failed: " + prepareErr.Error()
		return checked
	}
	checked.Prepared = true
	checked.DiscoveredReleaseID = imported.ReleaseID
	checked.AutomaticStatus = "prepared"
	if stateErr := s.markPreparedReleaseState(ctx, imported.ReleaseID); stateErr != nil {
		checked.AutomaticStatus = "paused"
		checked.AutomaticMessage = "could not determine whether the prepared release is ready: " + stateErr.Error()
	} else if target.Project.AutoBuildPolicy == "never" {
		checked.AutomaticStatus = "build-disabled"
		checked.AutomaticMessage = "this project's auto-build policy is Never"
	} else if built, buildErr := s.buildIfReviewFree(ctx, target, imported.ReleaseID, log); buildErr != nil {
		checked.AutomaticStatus = "paused"
		checked.AutomaticMessage = buildErr.Error()
		log("Automatic build paused: " + buildErr.Error() + "\n")
	} else {
		checked.Built = built
		setAutomaticBuildOutcome(&checked, built)
	}
	if persistErr := s.persistAutomaticOutcome(ctx, imported.ReleaseID, checked); persistErr != nil {
		log("Could not save automatic handling outcome: " + persistErr.Error() + "\n")
	}
	return checked
}

func (s *Service) markPreparedReleaseState(ctx context.Context, releaseID string) error {
	for attempt := 0; attempt < 3; attempt++ {
		release, err := s.Library.GetRelease(ctx, releaseID)
		if err != nil {
			return err
		}
		if len(releaseReviewIssues(release.Document)) > 0 || release.State == "ready" ||
			release.State == "built" {
			return nil
		}
		_, err = s.Library.PatchReleaseConfiguration(ctx, releaseID, release.Revision,
			map[string]any{"state": "ready"})
		if errors.Is(err, library.ErrConflict) {
			continue
		}
		return err
	}
	return library.ErrConflict
}

func (s *Service) persistAutomaticOutcome(ctx context.Context, releaseID string, result Result) error {
	for attempt := 0; attempt < 3; attempt++ {
		release, err := s.Library.GetRelease(ctx, releaseID)
		if err != nil {
			return err
		}
		update := cloneObject(object(release.Document["update"]))
		if update == nil {
			update = map[string]any{"strategy": StrategyManual}
		}
		update["lastAutomaticStatus"] = result.AutomaticStatus
		update["lastAutomaticMessage"] = result.AutomaticMessage
		_, err = s.Library.PatchReleaseConfiguration(ctx, releaseID, release.Revision,
			map[string]any{"update": update})
		if errors.Is(err, library.ErrConflict) {
			continue
		}
		return err
	}
	return library.ErrConflict
}

func (s *Service) checkGitHub(ctx context.Context, target checkTarget, log func(string)) (Result, error) {
	if s.GitHub == nil {
		return Result{}, fmt.Errorf("GitHub service is unavailable")
	}
	owner := stringValue(target.Update, "githubOwner")
	repository := stringValue(target.Update, "githubRepository")
	if owner == "" || repository == "" {
		return Result{}, fmt.Errorf("GitHub owner and repository must be configured")
	}
	log("Querying GitHub releases…\n")
	source, err := s.GitHub.Resolve(ctx, githubapi.ResolveRequest{
		URL:                "https://github.com/" + owner + "/" + repository,
		AssetRegex:         stringValue(target.Update, "githubAssetRegex"),
		IncludePrereleases: boolValue(target.Update, "githubIncludePrereleases"),
		CurrentVersion:     target.Version,
		CurrentReleaseID:   int64Value(target.Update, "githubReleaseId"),
		CurrentAssetID:     int64Value(target.Update, "githubAssetId"),
	})
	if err != nil {
		return Result{}, err
	}
	result := Result{Status: "no-update", Message: source.Message, UpdateAvailable: source.UpdateAvailable,
		DetectedVersion: source.DetectedVersion, Filename: source.Filename, SHA256: source.SHA256,
		DownloadURL: source.DownloadURL, ProviderReleaseID: source.ReleaseID, ProviderAssetID: source.AssetID,
		ProviderTag: source.Tag, PublisherDigest: source.PublisherDigest, Prerelease: source.Prerelease}
	if !source.UpdateAvailable {
		return result, nil
	}
	result.Status = "update"
	log("Downloading GitHub artifact and computing its SHA256…\n")
	record, err := s.GitHub.Download(ctx, source)
	if err != nil {
		return Result{}, err
	}
	if result.SHA256 != "" && !strings.EqualFold(result.SHA256, record.SHA256) {
		return Result{}, fmt.Errorf("GitHub publisher digest does not match the downloaded artifact")
	}
	result.SHA256 = record.SHA256
	result.Artifact = &record
	return result, nil
}

func (s *Service) persistObservation(ctx context.Context, target checkTarget, result Result) error {
	for attempt := 0; attempt < 3; attempt++ {
		release, err := s.Library.GetRelease(ctx, target.Release.ID)
		if err != nil {
			return err
		}
		update := cloneObject(object(release.Document["update"]))
		if update == nil {
			update = map[string]any{"strategy": StrategyManual}
		}
		now := timestamp(s.now())
		update["lastChecked"] = now
		update["lastCheckMessage"] = result.Message
		update["lastCheckFailed"] = result.Status == "error"
		update["signatureVerified"] = result.SignatureVerified
		if result.DetectedVersion != "" {
			update["detectedVersion"] = result.DetectedVersion
			update["detectedFilename"] = result.Filename
			update["detectedSha256"] = result.SHA256
			update["detectedUrl"] = result.DownloadURL
		}
		if stringValue(update, "strategy") == StrategyDirect && result.Status != "error" {
			update["directUrlEtag"] = result.ETag
			update["directUrlLastModified"] = result.DirectLastModified
			update["directUrlContentLength"] = strconvString(result.DirectContentLength)
			update["directUrlVendorValidatorName"] = result.DirectValidatorName
			update["directUrlVendorValidator"] = result.DirectValidatorValue
			if result.DirectLastSHA256 != "" {
				update["directUrlLastSha256"] = result.DirectLastSHA256
			}
			if result.DirectLastFullCheck != "" {
				update["directUrlLastFullCheck"] = result.DirectLastFullCheck
			}
		}
		if stringValue(update, "strategy") == StrategyGitHub && result.Status != "error" {
			update["githubReleaseId"] = strconvString(result.ProviderReleaseID)
			update["githubAssetId"] = strconvString(result.ProviderAssetID)
			update["githubTag"] = result.ProviderTag
			update["githubPublisherDigest"] = result.PublisherDigest
		}
		history, _ := release.Document["history"].([]any)
		history = append(history, map[string]any{"timestamp": now, "event": "update-check", "detail": result.Message})
		_, err = s.Library.PatchReleaseConfiguration(ctx, release.ID, release.Revision,
			map[string]any{"update": update, "history": history})
		if errors.Is(err, library.ErrConflict) {
			continue
		}
		if err != nil {
			return err
		}
		return s.persistNormalizedState(ctx, release.ID, update, result, now)
	}
	return library.ErrConflict
}

func (s *Service) persistNormalizedState(ctx context.Context, releaseID string, update map[string]any,
	result Result, checkedAt string) error {
	configuration := cloneObject(update)
	for _, key := range []string{"lastChecked", "lastCheckMessage", "lastCheckFailed", "signatureVerified",
		"lastAutomaticStatus", "lastAutomaticMessage",
		"detectedVersion", "detectedFilename", "detectedSha256", "detectedUrl",
		"directUrlLastSha256", "directUrlLastFullCheck", "githubEtag",
		"githubReleaseId", "githubAssetId", "githubTag", "githubPublisherDigest"} {
		delete(configuration, key)
	}
	raw, err := json.Marshal(configuration)
	if err != nil {
		return err
	}
	source, err := s.DB.Queries.GetUpdateSourceByRelease(ctx, releaseID)
	if errors.Is(err, sql.ErrNoRows) {
		source, err = s.DB.Queries.InsertUpdateSource(ctx, sqlcdb.InsertUpdateSourceParams{
			ID: uuid.NewString(), ReleaseID: releaseID, Strategy: stringValue(update, "strategy"), ConfigJson: string(raw)})
	} else if err == nil && (source.Strategy != stringValue(update, "strategy") || source.ConfigJson != string(raw)) {
		source, err = s.DB.Queries.UpdateUpdateSource(ctx, sqlcdb.UpdateUpdateSourceParams{
			Strategy: stringValue(update, "strategy"), ConfigJson: string(raw), ReleaseID: releaseID, Revision: source.Revision})
	}
	if err != nil {
		return err
	}
	lastError := ""
	if result.Status == "error" {
		lastError = result.Message
	}
	return s.DB.Queries.UpsertUpdateCheckState(ctx, sqlcdb.UpsertUpdateCheckStateParams{
		UpdateSourceID: source.ID, LastCheckedAt: checkedAt, LastMessage: result.Message,
		LastError: lastError, DetectedVersion: result.DetectedVersion, DetectedFilename: result.Filename,
		DetectedSha256: result.SHA256, DetectedUrl: result.DownloadURL, Etag: result.ETag,
		SignatureVerified: boolInt(result.SignatureVerified), JobID: "",
	})
}

func (s *Service) recordDiscovery(ctx context.Context, target checkTarget, result Result) (library.Release, error) {
	document := cloneObject(target.Release.Document)
	document["state"] = "discovered"
	document["sourceType"] = "unknown"
	document["originalSourceFilename"] = result.Filename
	document["sourceSha256"] = result.SHA256
	document["sourceUrl"] = result.DownloadURL
	debian := cloneObject(object(document["debian"]))
	debian["version"] = result.DetectedVersion
	document["debian"] = debian
	for _, key := range []string{"sourceArtifactId", "builtArtifactIds", "producedPackages", "buildRecords",
		"lastBuildLog", "createdAt", "modifiedAt", "id", "revision"} {
		delete(document, key)
	}
	document["buildStatus"] = "never-built"
	acquisition := cloneObject(object(document["acquisition"]))
	if acquisition == nil {
		acquisition = map[string]any{}
	}
	acquisition["originalUrl"] = result.DownloadURL
	acquisition["publisherDigest"] = result.SHA256
	acquisition["publisherVerified"] = result.SignatureVerified || result.PublisherDigest != ""
	switch stringValue(target.Update, "strategy") {
	case StrategyDirect:
		acquisition["kind"] = "direct-url"
	case StrategyAPT:
		acquisition["kind"] = "apt-repository"
	case StrategyRPM:
		acquisition["kind"] = "rpm-repository"
	case StrategyGitHub:
		acquisition["kind"] = "github-release"
		acquisition["githubOwner"] = stringValue(target.Update, "githubOwner")
		acquisition["githubRepository"] = stringValue(target.Update, "githubRepository")
		acquisition["githubReleaseId"] = result.ProviderReleaseID
		acquisition["githubAssetId"] = result.ProviderAssetID
		acquisition["githubTag"] = result.ProviderTag
		acquisition["githubAssetName"] = result.Filename
		acquisition["githubPrerelease"] = result.Prerelease
	}
	document["acquisition"] = acquisition
	return s.Library.CreateDiscoveredRelease(ctx, target.Project.ID, document)
}

func (s *Service) prepare(ctx context.Context, target checkTarget, result Result, log func(string)) (library.ImportResult, error) {
	record := result.Artifact
	if record == nil {
		downloadURL, err := url.Parse(result.DownloadURL)
		if err != nil || validateHTTPURL(downloadURL) != nil {
			return library.ImportResult{}, fmt.Errorf("discovered release has an invalid download URL")
		}
		log(fmt.Sprintf("Downloading %s…\n", result.Filename))
		downloaded, err := s.downloadArtifact(ctx, downloadURL, result.Filename, "vendor")
		if err != nil {
			return library.ImportResult{}, err
		}
		record = &downloaded
	}
	if result.SHA256 != "" && !strings.EqualFold(result.SHA256, record.SHA256) {
		return library.ImportResult{}, fmt.Errorf("downloaded artifact SHA256 does not match signed publisher metadata")
	}
	acquisition := cloneObject(object(target.Release.Document["acquisition"]))
	if acquisition == nil {
		acquisition = map[string]any{}
	}
	acquisition["originalUrl"] = result.DownloadURL
	acquisition["publisherDigest"] = result.SHA256
	acquisition["publisherVerified"] = result.SignatureVerified || result.PublisherDigest != ""
	kind := stringValue(acquisition, "kind")
	switch stringValue(target.Update, "strategy") {
	case StrategyDirect:
		kind = "direct-url"
	case StrategyAPT:
		kind = "apt-repository"
	case StrategyRPM:
		kind = "rpm-repository"
	case StrategyGitHub:
		kind = "github-release"
		acquisition["githubOwner"] = stringValue(target.Update, "githubOwner")
		acquisition["githubRepository"] = stringValue(target.Update, "githubRepository")
		acquisition["githubReleaseId"] = result.ProviderReleaseID
		acquisition["githubAssetId"] = result.ProviderAssetID
		acquisition["githubTag"] = result.ProviderTag
		acquisition["githubAssetName"] = result.Filename
		acquisition["githubPrerelease"] = result.Prerelease
	}
	acquisition["kind"] = kind
	rawAcquisition, _ := json.Marshal(acquisition)
	rawUpdate, _ := json.Marshal(target.Update)
	log("Inspecting downloaded vendor artifact…\n")
	request := library.ImportRequest{
		ArtifactID: record.ID, ExistingProjectID: target.Project.ID, Version: result.DetectedVersion,
		ExpectedSHA256: result.SHA256, AcquisitionKind: kind,
		CanonicalIdentity: target.Project.SourceIdentity, Acquisition: rawAcquisition, Update: rawUpdate,
		GitHubAssetRegex:         stringValue(target.Update, "githubAssetRegex"),
		GitHubIncludePrereleases: boolValue(target.Update, "githubIncludePrereleases"),
	}
	if strings.HasPrefix(target.Release.SourceSHA256, "pending:") {
		request.PendingReleaseID = target.Release.ID
		request.PendingProjectCreated = boolValue(object(target.Release.Document["importJob"]),
			"projectCreated")
	}
	return s.Library.ImportArtifact(ctx, request)
}

func (s *Service) buildIfReviewFree(ctx context.Context, target checkTarget, releaseID string,
	log func(string)) (bool, error) {
	if !target.Project.RepoPublish {
		return false, fmt.Errorf("repository publishing is not enabled for this project")
	}
	release, err := s.Library.GetRelease(ctx, releaseID)
	if err != nil {
		return false, err
	}
	if releaseHasSuccessfulBuild(release) {
		log("Automatic build skipped: release already has a successful package.\n")
		return false, nil
	}
	if boolValue(release.Document, "pkgbuildManuallyModified") {
		if target.Project.AutoBuildPolicy == "ai" {
			return false, fmt.Errorf("AI review is required for the Custom PKGBUILD")
		}
		return false, fmt.Errorf("Custom PKGBUILDs require external review")
	}
	if blockers := automaticReviewBlockers(target.Release.Document, release.Document); len(blockers) > 0 {
		if target.Project.AutoBuildPolicy == "ai" {
			return false, fmt.Errorf("AI review is required: %s", strings.Join(blockers, "; "))
		}
		return false, fmt.Errorf("%s", strings.Join(blockers, "; "))
	}
	log("Running automatic review-free build…\n")
	_, err = s.Library.BuildRelease(ctx, releaseID, log, true)
	if err != nil {
		return false, err
	}
	return true, nil
}

func releaseHasSuccessfulBuild(release library.Release) bool {
	return stringValue(release.Document, "buildStatus") == "succeeded" ||
		len(stringValues(release.Document["builtArtifactIds"])) > 0
}

func setAutomaticBuildOutcome(result *Result, built bool) {
	if built {
		result.AutomaticStatus = "built"
		result.AutomaticMessage = "automatic review-free build completed"
		return
	}
	result.AutomaticStatus = "already-built"
	result.AutomaticMessage = "release already has a successful build"
}

func activeTrackingRelease(project library.Project) *library.Release {
	var newest *library.Release
	for index := range project.Releases {
		candidate := &project.Releases[index]
		if candidate.State == "discovered" || candidate.State == "preparing" {
			continue
		}
		if newest == nil || releaseVersionCompare(*candidate, *newest) > 0 ||
			releaseVersionCompare(*candidate, *newest) == 0 && candidate.CreatedAt > newest.CreatedAt {
			newest = candidate
		}
	}
	return newest
}

func releaseVersionCompare(left, right library.Release) int {
	if left.SourceType == "rpm" || right.SourceType == "rpm" {
		return rpmVersionCompare(left.VendorVersion, right.VendorVersion)
	}
	return debianVersionCompare(left.VendorVersion, right.VendorVersion)
}

func strconvString(value int64) string { return fmt.Sprintf("%d", value) }
func boolInt(value bool) int64 {
	if value {
		return 1
	}
	return 0
}
