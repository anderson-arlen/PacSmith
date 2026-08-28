package library

import (
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/inspect"
	"github.com/anderson-arlen/pacsmith/server/internal/recipe"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

type Service struct {
	DB        *sqlite.DB
	Artifacts *artifact.Registry
	WorkDir   string
	Repo      *repo.Service
}

func (s *Service) ListProjects(ctx context.Context) ([]Project, error) {
	rows, err := s.DB.Queries.ListProjects(ctx)
	if err != nil {
		return nil, err
	}
	out := make([]Project, 0, len(rows))
	for _, row := range rows {
		project, err := s.GetProject(ctx, row.ID)
		if err != nil {
			return nil, err
		}
		out = append(out, project)
	}
	return out, nil
}

func (s *Service) ListProjectSummaries(ctx context.Context) ([]Project, error) {
	rows, err := s.DB.Queries.ListProjects(ctx)
	if err != nil {
		return nil, err
	}
	iconRows, err := s.DB.Queries.ListReleaseIconArtifacts(ctx)
	if err != nil {
		return nil, err
	}
	iconArtifacts := make(map[string]string, len(iconRows))
	for _, row := range iconRows {
		iconArtifacts[row.ReleaseID] = row.ArtifactID
	}
	out := make([]Project, 0, len(rows))
	for _, row := range rows {
		project := projectFromRow(row)
		releases, err := s.DB.Queries.ListReleasesForProject(ctx, row.ID)
		if err != nil {
			return nil, err
		}
		for _, release := range releases {
			summary := releaseSummary(release)
			attachReleaseIconSummary(&summary, release.BodyJson, iconArtifacts[release.ID])
			project.Releases = append(project.Releases, summary)
		}
		out = append(out, project)
	}
	return out, nil
}

func attachReleaseIconSummary(release *Release, bodyJSON, artifactID string) {
	if artifactID == "" {
		return
	}
	var body struct {
		InstallMapping struct {
			Icon map[string]any `json:"icon"`
		} `json:"installMapping"`
	}
	if json.Unmarshal([]byte(bodyJSON), &body) != nil || len(body.InstallMapping.Icon) == 0 {
		return
	}
	document := map[string]any{
		"installMapping": map[string]any{"icon": body.InstallMapping.Icon},
	}
	if !releaseIconConfigured(document) {
		return
	}
	release.Document["installMapping"] = document["installMapping"]
	release.Document["iconArtifactId"] = artifactID
}

func (s *Service) GetProject(ctx context.Context, id string) (Project, error) {
	row, err := s.DB.Queries.GetProject(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return Project{}, ErrNotFound
	}
	if err != nil {
		return Project{}, err
	}
	project := projectFromRow(row)
	releases, err := s.DB.Queries.ListReleasesForProject(ctx, id)
	if err != nil {
		return Project{}, err
	}
	for _, release := range releases {
		full, err := s.GetRelease(ctx, release.ID)
		if err != nil {
			return Project{}, err
		}
		project.Releases = append(project.Releases, full)
	}
	return project, nil
}

func (s *Service) DeleteProject(ctx context.Context, id string) error {
	if _, err := s.GetProject(ctx, id); err != nil {
		return err
	}
	if err := s.DB.Queries.DeleteProject(ctx, id); err != nil {
		return err
	}
	if s.Repo != nil {
		_ = s.Repo.OnProjectDeleted(ctx, id)
	}
	return nil
}

func (s *Service) DeleteRelease(ctx context.Context, id string) error {
	if _, err := s.GetRelease(ctx, id); err != nil {
		return err
	}
	return s.DB.Queries.DeleteRelease(ctx, id)
}

func (s *Service) FindProject(ctx context.Context, idOrName string) (Project, error) {
	idOrName = strings.TrimSpace(idOrName)
	if idOrName == "" {
		return Project{}, ErrNotFound
	}
	if project, err := s.GetProject(ctx, idOrName); err == nil {
		return project, nil
	}
	projects, err := s.ListProjects(ctx)
	if err != nil {
		return Project{}, err
	}
	for _, project := range projects {
		if project.ID == idOrName || project.ArchPackageName == idOrName ||
			project.DisplayName == idOrName ||
			project.ArchPackageName == idOrName+"-bin" {
			return s.GetProject(ctx, project.ID)
		}
	}
	return Project{}, ErrNotFound
}

type ProjectPatch struct {
	Revision           int64   `json:"revision"`
	DisplayName        string  `json:"displayName"`
	ArchPackageName    string  `json:"archPackageName"`
	VendorName         string  `json:"vendorName"`
	AutoBuildPolicy    *string `json:"autoBuildPolicy"`
	CompileCachePolicy *string `json:"compileCachePolicy"`
}

func (s *Service) PatchProject(ctx context.Context, id string, patch ProjectPatch) (Project, error) {
	row, err := s.DB.Queries.GetProject(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return Project{}, ErrNotFound
	}
	if err != nil {
		return Project{}, err
	}
	revision := patch.Revision
	if revision == 0 {
		revision = row.Revision
	}
	display := row.DisplayName
	if strings.TrimSpace(patch.DisplayName) != "" {
		display = patch.DisplayName
	}
	arch := row.ArchPackageName
	if strings.TrimSpace(patch.ArchPackageName) != "" {
		arch = patch.ArchPackageName
	}
	vendor := row.VendorName
	if patch.VendorName != "" {
		vendor = patch.VendorName
	}
	autoBuildPolicy := row.AutoBuildPolicy
	if patch.AutoBuildPolicy != nil {
		autoBuildPolicy = strings.TrimSpace(*patch.AutoBuildPolicy)
	}
	if autoBuildPolicy != "never" && autoBuildPolicy != "review_free" && autoBuildPolicy != "ai" {
		return Project{}, fmt.Errorf("%w: autoBuildPolicy must be never, review_free, or ai", ErrInvalid)
	}
	compileCachePolicy := row.CompileCachePolicy
	if patch.CompileCachePolicy != nil {
		compileCachePolicy = strings.TrimSpace(*patch.CompileCachePolicy)
	}
	if compileCachePolicy != "reuse" && compileCachePolicy != "clear_after_success" &&
		compileCachePolicy != "disabled" {
		return Project{}, fmt.Errorf("%w: compileCachePolicy must be reuse, clear_after_success, or disabled", ErrInvalid)
	}
	_, err = s.DB.Queries.UpdateProject(ctx, sqlcdb.UpdateProjectParams{
		DisplayName:        display,
		ArchPackageName:    arch,
		VendorName:         vendor,
		SourceIdentity:     row.SourceIdentity,
		IconArtifactID:     row.IconArtifactID,
		IconSha256:         row.IconSha256,
		HistoryJson:        row.HistoryJson,
		AutoBuildPolicy:    autoBuildPolicy,
		CompileCachePolicy: compileCachePolicy,
		ModifiedAt:         nowUTC(),
		ID:                 row.ID,
		Revision:           revision,
	})
	if errors.Is(err, sql.ErrNoRows) {
		return Project{}, ErrConflict
	}
	if err != nil {
		return Project{}, err
	}
	return s.GetProject(ctx, id)
}

func (s *Service) SaveRelease(ctx context.Context, releaseID string, revision int64, document map[string]any) (Release, error) {
	row, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, err
	}
	if revision == 0 {
		revision = row.Revision
	}
	delete(document, "installedVersion")
	delete(document, "installedReleaseId")
	delete(document, "externallyInstalled")
	attachInspectedRelease(document)
	_ = s.persistSigningKeys(ctx, releaseID, document,
		inspect.InspectScripts(maintainerScriptsFromDocument(document)).SigningKeys, nil, "")
	attachIdentityVariables(row, document)
	raw, err := json.Marshal(document)
	if err != nil {
		return Release{}, err
	}
	state := row.State
	if value := stringValue(document, "state"); value != "" {
		state = value
	}
	updated, err := s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
		State:            state,
		SourceType:       row.SourceType,
		VendorVersion:    row.VendorVersion,
		OriginalFilename: row.OriginalFilename,
		SourceSha256:     row.SourceSha256,
		SourceArtifactID: row.SourceArtifactID,
		ArchPackageName:  row.ArchPackageName,
		ArchPkgrel:       row.ArchPkgrel,
		BodyJson:         string(raw),
		ModifiedAt:       nowUTC(),
		ID:               row.ID,
		Revision:         revision,
	})
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrConflict
	}
	if err != nil {
		return Release{}, err
	}
	return releaseDocument(updated), nil
}

func (s *Service) CreateDiscoveredRelease(ctx context.Context, projectID string,
	document map[string]any) (Release, error) {
	project, err := s.GetProject(ctx, projectID)
	if err != nil {
		return Release{}, err
	}
	debian, ok := mapValue(document, "debian")
	if !ok {
		return Release{}, ErrInvalid
	}
	version := strings.TrimSpace(stringValue(debian, "version"))
	filename := strings.TrimSpace(stringValue(document, "originalSourceFilename"))
	sha := strings.ToLower(strings.TrimSpace(stringValue(document, "sourceSha256")))
	sourceURL := strings.TrimSpace(stringValue(document, "sourceUrl"))
	decodedSHA, decodeErr := hex.DecodeString(sha)
	if version == "" || filename == "" || sourceURL == "" ||
		decodeErr != nil || len(decodedSHA) != sha256.Size {
		return Release{}, ErrInvalid
	}
	if existing, getErr := s.DB.Queries.GetReleaseByProjectSHA256(
		ctx, sqlcdb.GetReleaseByProjectSHA256Params{
			ProjectID: projectID, SourceSha256: sha,
		}); getErr == nil {
		return releaseDocument(existing), nil
	} else if !errors.Is(getErr, sql.ErrNoRows) {
		return Release{}, getErr
	}

	releaseID := uuid.NewString()
	document = cloneObject(document)
	document["id"] = releaseID
	document["projectId"] = projectID
	document["state"] = "discovered"
	document["sourceType"] = "unknown"
	document["archPackageName"] = project.ArchPackageName
	now := nowUTC()
	document["createdAt"] = now
	document["modifiedAt"] = now
	raw, err := json.Marshal(document)
	if err != nil {
		return Release{}, err
	}
	release, err := s.DB.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID: releaseID, ProjectID: projectID, State: "discovered", SourceType: "unknown",
		VendorVersion: version, OriginalFilename: filename, SourceSha256: sha,
		SourceArtifactID: sql.NullString{}, ArchPackageName: project.ArchPackageName,
		ArchPkgrel: 1, BodyJson: string(raw), CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		return Release{}, err
	}
	return releaseDocument(release), nil
}

func (s *Service) Reanalyze(ctx context.Context, releaseID string) (ImportResult, error) {
	row, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if errors.Is(err, sql.ErrNoRows) {
		return ImportResult{}, ErrNotFound
	}
	if err != nil {
		return ImportResult{}, err
	}
	if !row.SourceArtifactID.Valid {
		return ImportResult{}, fmt.Errorf("%w: release has no source artifact", ErrInvalid)
	}
	source, err := s.Artifacts.Get(ctx, row.SourceArtifactID.String)
	if err != nil {
		return ImportResult{}, err
	}
	path, err := s.Artifacts.Store.Path(source.SHA256)
	if err != nil {
		return ImportResult{}, err
	}
	analysis, err := inspect.AnalyzeArtifact(path, source.OriginalFilename)
	if err != nil {
		return ImportResult{}, fmt.Errorf("%w: %s", ErrInvalid, err.Error())
	}
	previous := releaseDocument(row).Document
	alignIntegrationIconName(&analysis, row.ArchPackageName)
	recipeRel := recipeFromAnalysis(row.ProjectID, row.ID, source.OriginalFilename, source.SHA256, analysis)
	recipeRel.ArchPackageName = row.ArchPackageName
	recipeRel.DisplayName = firstNonEmpty(stringValue(previous, "displayName"), row.ArchPackageName)
	pkgbuild := recipe.Generate(recipeRel)
	document, err := analysisDocument(source.OriginalFilename, source.SHA256, pkgbuild, analysis)
	if err != nil {
		return ImportResult{}, err
	}
	var body map[string]any
	if err := json.Unmarshal([]byte(document), &body); err != nil {
		return ImportResult{}, err
	}
	body["id"] = row.ID
	body["projectId"] = row.ProjectID
	body["displayName"] = recipeRel.DisplayName
	body["archPackageName"] = row.ArchPackageName
	body["vendorName"] = firstNonEmpty(analysis.Metadata.Maintainer, stringValue(previous, "vendorName"))
	body["archPkgrel"] = row.ArchPkgrel
	body["state"] = "needs-review"
	body["formatVersion"] = 1
	body["createdAt"] = row.CreatedAt
	if acquisition, ok := previous["acquisition"]; ok {
		body["acquisition"] = acquisition
	}
	if sourceURL := stringValue(previous, "sourceUrl"); sourceURL != "" {
		body["sourceUrl"] = sourceURL
	}
	if history := previous["history"]; history != nil {
		body["history"] = history
	}
	if update, ok := mapValue(previous, "update"); ok {
		body["update"] = cloneObject(update)
		mergeUpdateCandidateLists(body["update"].(map[string]any), updateConfigurationJSON(analysis))
	}
	attachInspectedRelease(body)
	fromDoc := recipeFromDocument(Release{
		ID:              row.ID,
		ProjectID:       row.ProjectID,
		ArchPackageName: row.ArchPackageName,
		SourceType:      sourceTypeName(analysis.Type),
		SourceSHA256:    source.SHA256,
		VendorVersion:   analysis.Metadata.Version,
		Document:        body,
	})
	fromDoc.ArchPkgrel = int(row.ArchPkgrel)
	body["generatedPkgbuild"] = recipe.Generate(fromDoc)
	body["identityVariables"] = recipe.IdentityVariables(fromDoc)
	_ = s.persistSigningKeys(ctx, row.ID, body, analysis.SigningKeys, nil, "")
	raw, err := json.Marshal(body)
	if err != nil {
		return ImportResult{}, err
	}
	if _, err := s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
		State:            "needs-review",
		SourceType:       sourceTypeName(analysis.Type),
		VendorVersion:    analysis.Metadata.Version,
		OriginalFilename: source.OriginalFilename,
		SourceSha256:     source.SHA256,
		SourceArtifactID: row.SourceArtifactID,
		ArchPackageName:  row.ArchPackageName,
		ArchPkgrel:       row.ArchPkgrel,
		BodyJson:         string(raw),
		ModifiedAt:       nowUTC(),
		ID:               row.ID,
		Revision:         row.Revision,
	}); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ImportResult{}, ErrConflict
		}
		return ImportResult{}, err
	}
	s.replaceInspectedIcon(ctx, row.ID, analysis)
	_ = s.associateSigningKeyArtifacts(ctx, row.ID, body)
	return ImportResult{ProjectID: row.ProjectID, ReleaseID: row.ID}, nil
}

func (s *Service) GetRelease(ctx context.Context, id string) (Release, error) {
	row, err := s.DB.Queries.GetRelease(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, err
	}
	release := releaseDocument(row)
	attachInspectedRelease(release.Document)
	artifacts, err := s.DB.Queries.ListReleaseArtifacts(ctx, id)
	if err != nil {
		return Release{}, err
	}
	var sourceID, iconID string
	var built []string
	for _, item := range artifacts {
		switch item.Role {
		case "source":
			sourceID = item.ArtifactID
		case "icon":
			iconID = item.ArtifactID
		case "built_package":
			built = append(built, item.ArtifactID)
		}
	}
	if !releaseIconConfigured(release.Document) {
		iconID = ""
	}
	release.Document["sourceArtifactId"] = sourceID
	release.Document["iconArtifactId"] = iconID
	release.Document["builtArtifactIds"] = built
	builds, err := s.DB.Queries.ListBuildsForRelease(ctx, id)
	if err != nil {
		return Release{}, err
	}
	if err := s.attachBuildRecords(ctx, &release, builds); err != nil {
		return Release{}, err
	}
	return release, nil
}

type ImportRequest struct {
	ArtifactID               string          `json:"artifact_id"`
	ExistingProjectID        string          `json:"existing_project_id"`
	AcquisitionKind          string          `json:"acquisition_kind"`
	CanonicalIdentity        string          `json:"canonical_identity"`
	Acquisition              json.RawMessage `json:"acquisition"`
	GitHubAssetRegex         string          `json:"github_asset_regex"`
	GitHubIncludePrereleases bool            `json:"github_include_prereleases"`
	Update                   json.RawMessage `json:"update"`
	TrustedSigningKey        string          `json:"trusted_signing_key"`
	TrustedSigningKeySource  string          `json:"trusted_signing_key_source"`
}

type ImportResult struct {
	ProjectID      string `json:"project_id"`
	ReleaseID      string `json:"release_id"`
	ProjectCreated bool   `json:"project_created"`
	Duplicate      bool   `json:"duplicate"`
}

func (s *Service) ImportArtifact(ctx context.Context, req ImportRequest) (ImportResult, error) {
	record, err := s.Artifacts.Get(ctx, req.ArtifactID)
	if err != nil {
		if errors.Is(err, artifact.ErrNotFound) {
			return ImportResult{}, fmt.Errorf("%w: artifact", ErrNotFound)
		}
		return ImportResult{}, err
	}
	path, err := s.Artifacts.Store.Path(record.SHA256)
	if err != nil {
		return ImportResult{}, err
	}
	analysis, err := inspect.AnalyzeArtifact(path, record.OriginalFilename)
	if err != nil {
		return ImportResult{}, fmt.Errorf("%w: %s", ErrInvalid, err.Error())
	}

	identity := strings.TrimSpace(req.CanonicalIdentity)
	if identity == "" {
		identity = "local:" + analysis.Metadata.Package
	}

	var project sqlcdb.Project
	created := false
	if req.ExistingProjectID != "" {
		project, err = s.DB.Queries.GetProject(ctx, req.ExistingProjectID)
		if errors.Is(err, sql.ErrNoRows) {
			return ImportResult{}, fmt.Errorf("%w: project", ErrNotFound)
		}
		if err != nil {
			return ImportResult{}, err
		}
	} else {
		matches, err := s.DB.Queries.ListProjectsBySourceIdentity(ctx, identity)
		if err != nil {
			return ImportResult{}, err
		}
		if len(matches) > 0 {
			project = matches[0]
		} else {
			created = true
			now := nowUTC()
			pkg := recipe.SanitizePackageName(analysis.Metadata.Package)
			if pkg == "" {
				pkg = "vendor-package"
			}
			project, err = s.DB.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
				ID:              uuid.NewString(),
				DisplayName:     preferredName(analysis),
				ArchPackageName: pkg + "-bin",
				VendorName:      analysis.Metadata.Maintainer,
				SourceIdentity:  identity,
				IconArtifactID:  sql.NullString{},
				IconSha256:      "",
				HistoryJson:     "[]",
				CreatedAt:       now,
				ModifiedAt:      now,
			})
			if err != nil {
				return ImportResult{}, err
			}
		}
	}

	existing, err := s.DB.Queries.GetReleaseByProjectSHA256(ctx, sqlcdb.GetReleaseByProjectSHA256Params{
		ProjectID:    project.ID,
		SourceSha256: record.SHA256,
	})
	foundExisting := err == nil
	if foundExisting && existing.State != "discovered" {
		return ImportResult{ProjectID: project.ID, ReleaseID: existing.ID, Duplicate: true}, nil
	}
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return ImportResult{}, err
	}

	releaseID := uuid.NewString()
	if foundExisting {
		releaseID = existing.ID
	}
	alignIntegrationIconName(&analysis, project.ArchPackageName)
	recipeRel := recipeFromAnalysis(project.ID, releaseID, record.OriginalFilename, record.SHA256, analysis)
	recipeRel.ArchPackageName = project.ArchPackageName
	recipeRel.DisplayName = project.DisplayName
	pkgbuild := recipe.Generate(recipeRel)
	recipeRel.ID = releaseID
	document, err := analysisDocument(record.OriginalFilename, record.SHA256, pkgbuild, analysis)
	if err != nil {
		return ImportResult{}, err
	}
	var body map[string]any
	if err := json.Unmarshal([]byte(document), &body); err != nil {
		return ImportResult{}, err
	}
	body["identityVariables"] = recipe.IdentityVariables(recipeRel)
	body["generatedPkgbuild"] = pkgbuild
	body["formatVersion"] = 1
	body["displayName"] = project.DisplayName
	body["archPackageName"] = project.ArchPackageName
	body["vendorName"] = project.VendorName
	body["state"] = "needs-review"
	if req.CanonicalIdentity == "" {
		req.CanonicalIdentity = identity
	}
	applyAcquisition(body, req)
	if len(req.Update) > 0 && string(req.Update) != "null" {
		mergeImportedUpdate(body, req.Update)
	}
	applyGitHubImportOptions(body, req)
	attachInspectedRelease(body)
	if previous := s.newestPreparedDocument(ctx, project.ID, releaseID); previous != nil {
		carryForwardRelease(previous, body)
		if len(req.Update) == 0 || string(req.Update) == "null" {
			inheritUpdateConfiguration(previous, body)
		}
	}
	pkgrel := 1
	if foundExisting {
		pkgrel = int(existing.ArchPkgrel)
		if pkgrel < 1 {
			pkgrel = 1
		}
	} else {
		pkgrel = s.nextPkgrel(ctx, project.ID, analysis.Metadata.Version)
	}
	body["archPkgrel"] = pkgrel
	fromDoc := recipeFromDocument(Release{
		ID:              releaseID,
		ProjectID:       project.ID,
		ArchPackageName: project.ArchPackageName,
		SourceType:      sourceTypeName(analysis.Type),
		SourceSHA256:    record.SHA256,
		VendorVersion:   analysis.Metadata.Version,
		Document:        body,
	})
	fromDoc.ArchPkgrel = pkgrel
	body["generatedPkgbuild"] = recipe.Generate(fromDoc)
	body["identityVariables"] = recipe.IdentityVariables(fromDoc)
	trusted, _ := decodeTrustedKey(req.TrustedSigningKey)
	_ = s.persistSigningKeys(ctx, "", body, analysis.SigningKeys, trusted, req.TrustedSigningKeySource)
	raw, err := json.Marshal(body)
	if err != nil {
		return ImportResult{}, err
	}

	now := nowUTC()
	var release sqlcdb.Release
	if foundExisting {
		release, err = s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
			State:            "needs-review",
			SourceType:       sourceTypeName(analysis.Type),
			VendorVersion:    analysis.Metadata.Version,
			OriginalFilename: record.OriginalFilename,
			SourceSha256:     record.SHA256,
			SourceArtifactID: nullString(record.ID),
			ArchPackageName:  project.ArchPackageName,
			ArchPkgrel:       int64(pkgrel),
			BodyJson:         string(raw),
			ModifiedAt:       now,
			ID:               existing.ID,
			Revision:         existing.Revision,
		})
		if errors.Is(err, sql.ErrNoRows) {
			return ImportResult{}, ErrConflict
		}
	} else {
		release, err = s.DB.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
			ID:               releaseID,
			ProjectID:        project.ID,
			State:            "needs-review",
			SourceType:       sourceTypeName(analysis.Type),
			VendorVersion:    analysis.Metadata.Version,
			OriginalFilename: record.OriginalFilename,
			SourceSha256:     record.SHA256,
			SourceArtifactID: nullString(record.ID),
			ArchPackageName:  project.ArchPackageName,
			ArchPkgrel:       int64(pkgrel),
			BodyJson:         string(raw),
			CreatedAt:        now,
			ModifiedAt:       now,
		})
	}
	if err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "unique") {
			existing, getErr := s.DB.Queries.GetReleaseByProjectSHA256(ctx, sqlcdb.GetReleaseByProjectSHA256Params{
				ProjectID:    project.ID,
				SourceSha256: record.SHA256,
			})
			if getErr == nil {
				return ImportResult{ProjectID: project.ID, ReleaseID: existing.ID, Duplicate: true}, nil
			}
		}
		return ImportResult{}, err
	}
	if err := s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID:  release.ID,
		ArtifactID: record.ID,
		Role:       "source",
	}); err != nil {
		return ImportResult{}, err
	}
	s.replaceInspectedIcon(ctx, release.ID, analysis)
	_ = s.associateSigningKeyArtifacts(ctx, release.ID, body)
	return ImportResult{
		ProjectID:      project.ID,
		ReleaseID:      release.ID,
		ProjectCreated: created,
	}, nil
}

func (s *Service) File(ctx context.Context, releaseID, name string) (string, string, error) {
	release, err := s.GetRelease(ctx, releaseID)
	if err != nil {
		return "", "", err
	}
	switch name {
	case "PKGBUILD":
		if boolValue(release.Document, "pkgbuildManuallyModified") {
			if custom := stringValue(release.Document, "customPkgbuild"); custom != "" {
				return custom, "text/plain", nil
			}
		}
		return stringValue(release.Document, "generatedPkgbuild"), "text/plain", nil
	case "pacsmith.vars":
		return identityVariablesFor(release), "text/plain", nil
	default:
		if files, ok := mapValue(release.Document, "customFiles"); ok {
			if contents, exists := files[name].(string); exists {
				return contents, "text/plain", nil
			}
		}
		if lifecycle, ok := release.Document["lifecycleScript"].(map[string]any); ok {
			fileName := stringValue(lifecycle, "fileName")
			if fileName == name || (fileName == "" && strings.HasSuffix(name, ".install")) {
				return stringValue(lifecycle, "contents"), "text/plain", nil
			}
		}
		return "", "", fmt.Errorf("%w: unknown file", ErrInvalid)
	}
}

func (s *Service) PutFile(ctx context.Context, releaseID, name, contents string, revision int64, manuallyModified *bool) (Release, error) {
	row, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, err
	}
	if revision == 0 {
		revision = row.Revision
	}
	var body map[string]any
	if err := json.Unmarshal([]byte(row.BodyJson), &body); err != nil {
		return Release{}, err
	}
	switch {
	case name == "PKGBUILD":
		custom := true
		if manuallyModified != nil {
			custom = *manuallyModified
		}
		if custom {
			body["customPkgbuild"] = contents
			body["pkgbuildManuallyModified"] = true
		} else {
			body["generatedPkgbuild"] = contents
			body["pkgbuildManuallyModified"] = false
		}
	case strings.HasSuffix(name, ".install"):
		validation := recipe.ValidateLifecycle(contents)
		if !validation.Passed {
			return Release{}, fmt.Errorf("%w: %s", ErrInvalid, validation.Message())
		}
		lifecycle, ok := mapValue(body, "lifecycleScript")
		if !ok {
			lifecycle = map[string]any{}
		}
		previous := stringValue(lifecycle, "contents")
		lifecycle["fileName"] = name
		lifecycle["contents"] = contents
		lifecycle["validationPassed"] = true
		lifecycle["validationMessage"] = validation.Message()
		if previous != contents {
			lifecycle["acknowledgedFingerprint"] = ""
			lifecycle["manuallyModified"] = true
		}
		body["lifecycleScript"] = lifecycle
	default:
		if !boolValue(body, "pkgbuildManuallyModified") {
			return Release{}, fmt.Errorf("%w: support files belong to Custom PKGBUILD mode", ErrInvalid)
		}
		if err := validateCustomFileName(name); err != nil {
			return Release{}, err
		}
		files, ok := mapValue(body, "customFiles")
		if !ok {
			files = map[string]any{}
		}
		files[name] = contents
		body["customFiles"] = files
	}
	attachIdentityVariables(row, body)
	raw, err := json.Marshal(body)
	if err != nil {
		return Release{}, err
	}
	updated, err := s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
		State:            row.State,
		SourceType:       row.SourceType,
		VendorVersion:    row.VendorVersion,
		OriginalFilename: row.OriginalFilename,
		SourceSha256:     row.SourceSha256,
		SourceArtifactID: row.SourceArtifactID,
		ArchPackageName:  row.ArchPackageName,
		ArchPkgrel:       row.ArchPkgrel,
		BodyJson:         string(raw),
		ModifiedAt:       nowUTC(),
		ID:               row.ID,
		Revision:         revision,
	})
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrConflict
	}
	if err != nil {
		return Release{}, err
	}
	return releaseDocument(updated), nil
}

func (s *Service) DeleteFile(ctx context.Context, releaseID, name string, revision int64) (Release, error) {
	if err := validateCustomFileName(name); err != nil {
		return Release{}, err
	}
	row, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrNotFound
	}
	if err != nil {
		return Release{}, err
	}
	if revision == 0 {
		revision = row.Revision
	}
	var body map[string]any
	if err := json.Unmarshal([]byte(row.BodyJson), &body); err != nil {
		return Release{}, err
	}
	files, ok := mapValue(body, "customFiles")
	if !ok {
		return Release{}, fmt.Errorf("%w: unknown file", ErrNotFound)
	}
	if _, exists := files[name]; !exists {
		return Release{}, fmt.Errorf("%w: unknown file", ErrNotFound)
	}
	delete(files, name)
	body["customFiles"] = files
	raw, err := json.Marshal(body)
	if err != nil {
		return Release{}, err
	}
	updated, err := s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
		State: row.State, SourceType: row.SourceType, VendorVersion: row.VendorVersion,
		OriginalFilename: row.OriginalFilename, SourceSha256: row.SourceSha256,
		SourceArtifactID: row.SourceArtifactID, ArchPackageName: row.ArchPackageName,
		ArchPkgrel: row.ArchPkgrel, BodyJson: string(raw), ModifiedAt: nowUTC(),
		ID: row.ID, Revision: revision,
	})
	if errors.Is(err, sql.ErrNoRows) {
		return Release{}, ErrConflict
	}
	if err != nil {
		return Release{}, err
	}
	return releaseDocument(updated), nil
}

func validateCustomFileName(name string) error {
	if name == "" || name != filepath.Base(name) || name == "." || name == ".." ||
		name == "PKGBUILD" || name == "pacsmith.vars" || strings.ContainsAny(name, "\x00/\\") {
		return fmt.Errorf("%w: invalid custom support filename", ErrInvalid)
	}
	return nil
}

type BuildResult struct {
	Status    string   `json:"status"`
	Log       string   `json:"-"`
	Artifacts []string `json:"artifact_ids"`
}

func (s *Service) BuildRelease(ctx context.Context, releaseID string,
	logOutput func(string), automatic bool) (BuildResult, error) {
	row, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if errors.Is(err, sql.ErrNoRows) {
		return BuildResult{}, ErrNotFound
	}
	if err != nil {
		return BuildResult{}, err
	}
	if !row.SourceArtifactID.Valid {
		return BuildResult{}, fmt.Errorf("%w: release has no source artifact", ErrInvalid)
	}
	source, err := s.Artifacts.Get(ctx, row.SourceArtifactID.String)
	if err != nil {
		return BuildResult{}, err
	}
	objectPath, err := s.Artifacts.Store.Path(source.SHA256)
	if err != nil {
		return BuildResult{}, err
	}
	pkgbuild, _, err := s.File(ctx, releaseID, "PKGBUILD")
	if err != nil {
		return BuildResult{}, err
	}
	vars, _, err := s.File(ctx, releaseID, "pacsmith.vars")
	if err != nil {
		return BuildResult{}, err
	}
	document := releaseDocument(row).Document
	isCustom := customBuild(document)
	work := filepath.Join(s.WorkDir, "releases", releaseID)
	if err := resetBuildWorkspace(work, isCustom); err != nil {
		return BuildResult{}, err
	}
	if !isCustom {
		defer os.RemoveAll(work)
	}
	if err := os.MkdirAll(filepath.Join(work, "sources"), 0o700); err != nil {
		return BuildResult{}, err
	}
	sourceDest := filepath.Join(work, "sources", row.OriginalFilename)
	if err := copyFile(objectPath, sourceDest); err != nil {
		return BuildResult{}, err
	}
	link := filepath.Join(work, row.OriginalFilename)
	_ = os.Remove(link)
	if err := os.Symlink(filepath.Join("sources", row.OriginalFilename), link); err != nil {
		if err := copyFile(sourceDest, link); err != nil {
			return BuildResult{}, err
		}
	}
	if err := s.writeReleaseIcon(ctx, row, work); err != nil {
		return BuildResult{}, err
	}
	if lifecycle, ok := mapValue(releaseDocument(row).Document, "lifecycleScript"); ok {
		fileName := stringValue(lifecycle, "fileName")
		contents := stringValue(lifecycle, "contents")
		if fileName != "" && contents != "" {
			if err := os.WriteFile(filepath.Join(work, filepath.Base(fileName)), []byte(contents), 0o600); err != nil {
				return BuildResult{}, err
			}
		}
	}
	if files, ok := mapValue(releaseDocument(row).Document, "customFiles"); ok {
		for name, value := range files {
			contents, valid := value.(string)
			if !valid {
				return BuildResult{}, fmt.Errorf("%w: custom support file is not text", ErrInvalid)
			}
			if err := validateCustomFileName(name); err != nil {
				return BuildResult{}, err
			}
			if err := os.WriteFile(filepath.Join(work, name), []byte(contents), 0o600); err != nil {
				return BuildResult{}, err
			}
		}
	}

	row, vars, pkgbuild, err = s.prepareBuildIdentity(ctx, row, pkgbuild, vars)
	if err != nil {
		return BuildResult{}, err
	}
	if err := os.WriteFile(filepath.Join(work, "PKGBUILD"), []byte(pkgbuild), 0o600); err != nil {
		return BuildResult{}, err
	}
	if err := os.WriteFile(filepath.Join(work, "pacsmith.vars"), []byte(vars), 0o600); err != nil {
		return BuildResult{}, err
	}

	settings, err := s.DB.Queries.GetLibrarySettings(ctx)
	if err != nil {
		return BuildResult{}, err
	}
	project, err := s.DB.Queries.GetProject(ctx, row.ProjectID)
	if err != nil {
		return BuildResult{}, err
	}
	execution := buildExecution{
		ReleaseID: releaseID, ProjectID: row.ProjectID, WorkDir: work,
		Parallelism:        int(settings.BuildParallelism),
		CompileCachePolicy: project.CompileCachePolicy, LogOutput: logOutput,
	}
	var logText string
	if isCustom {
		if logOutput != nil {
			logOutput("Starting isolated rootless Podman build…\n")
		}
		logText, err = runContainerBuild(ctx, execution)
	} else {
		if os.Geteuid() == 0 {
			return BuildResult{}, fmt.Errorf("%w: refusing to run makepkg as root", ErrInvalid)
		}
		logText, err = runNativeBuild(ctx, execution)
	}
	status := "succeeded"
	if err != nil {
		status = "failed"
	}
	if isCustom && status == "succeeded" {
		defer os.RemoveAll(work)
	}
	if isCustom && status == "succeeded" &&
		project.CompileCachePolicy == "clear_after_success" {
		defer os.RemoveAll(projectCompileCacheDir(work, row.ProjectID))
	}
	now := nowUTC()
	build, buildErr := s.DB.Queries.InsertBuild(ctx, sqlcdb.InsertBuildParams{
		ID:         uuid.NewString(),
		ReleaseID:  releaseID,
		Status:     status,
		LogText:    logText,
		StartedAt:  nullString(now),
		FinishedAt: nullString(now),
	})
	if buildErr != nil {
		return BuildResult{}, buildErr
	}
	result := BuildResult{Status: status, Log: logText}
	if status != "succeeded" {
		if summaryErr := s.recordBuildSummary(ctx, releaseID, status, logText, nil, automatic); summaryErr != nil {
			return result, fmt.Errorf("makepkg failed; record build summary: %w", summaryErr)
		}
		return result, fmt.Errorf("makepkg failed")
	}
	entries, _ := filepath.Glob(filepath.Join(work, "*.pkg.tar.*"))
	for _, path := range entries {
		if strings.HasSuffix(path, ".sig") {
			continue
		}
		file, err := os.Open(path)
		if err != nil {
			continue
		}
		record, putErr := s.Artifacts.Put(ctx, filepath.Base(path), "arch_package", file)
		_ = file.Close()
		if putErr != nil {
			continue
		}
		if err := s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
			ReleaseID:  releaseID,
			ArtifactID: record.ID,
			Role:       "built_package",
		}); err != nil {
			return result, err
		}
		if err := s.DB.Queries.InsertBuildArtifact(ctx, sqlcdb.InsertBuildArtifactParams{
			BuildID: build.ID, ArtifactID: record.ID,
		}); err != nil {
			return result, err
		}
		result.Artifacts = append(result.Artifacts, record.ID)
	}
	producedPackages := make([]string, 0, len(result.Artifacts))
	for _, artifactID := range result.Artifacts {
		record, getErr := s.Artifacts.Get(ctx, artifactID)
		if getErr == nil {
			producedPackages = append(producedPackages, record.OriginalFilename)
		}
	}
	if err := s.recordBuildSummary(ctx, releaseID, status, logText, producedPackages, automatic); err != nil {
		return result, err
	}
	if s.Repo != nil && len(result.Artifacts) > 0 {
		if err := s.Repo.PublishBuild(ctx, row.ProjectID, releaseID, result.Artifacts); err != nil {
			result.Log += "\nrepository publish: " + err.Error() + "\n"
			return result, err
		}
	}
	return result, nil
}

func resetBuildWorkspace(work string, preserveCustomSources bool) error {
	preserved := work + ".preserved-src"
	if err := os.RemoveAll(preserved); err != nil {
		return err
	}
	if preserveCustomSources {
		if err := os.Rename(filepath.Join(work, "src"), preserved); err != nil &&
			!errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	if err := os.RemoveAll(work); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Join(work, "sources"), 0o700); err != nil {
		return err
	}
	if preserveCustomSources {
		if err := os.Rename(preserved, filepath.Join(work, "src")); err != nil &&
			!errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	return nil
}

type buildLogWriter func(string)

func (writer buildLogWriter) Write(data []byte) (int, error) {
	writer(string(data))
	return len(data), nil
}

func buildParallelismArguments(parallelism int) []string {
	if parallelism < 1 {
		parallelism = 1
	}
	return []string{
		fmt.Sprintf("MAKEFLAGS=-j%d", parallelism),
		fmt.Sprintf("CMAKE_BUILD_PARALLEL_LEVEL=%d", parallelism),
	}
}

func (s *Service) SetReleaseIcon(ctx context.Context, releaseID, artifactID string) (Release, error) {
	if _, err := s.GetRelease(ctx, releaseID); err != nil {
		return Release{}, err
	}
	if _, err := s.Artifacts.Get(ctx, artifactID); err != nil {
		if errors.Is(err, artifact.ErrNotFound) {
			return Release{}, fmt.Errorf("%w: icon artifact", ErrNotFound)
		}
		return Release{}, err
	}
	if err := s.replaceIconArtifact(ctx, releaseID, artifactID); err != nil {
		return Release{}, err
	}
	return s.GetRelease(ctx, releaseID)
}

func (s *Service) replaceIconArtifact(ctx context.Context, releaseID, artifactID string) error {
	if err := s.DB.Queries.DeleteReleaseArtifactsByRole(ctx, sqlcdb.DeleteReleaseArtifactsByRoleParams{
		ReleaseID: releaseID,
		Role:      "icon",
	}); err != nil {
		return err
	}
	return s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID:  releaseID,
		ArtifactID: artifactID,
		Role:       "icon",
	})
}

func releaseIconConfigured(document map[string]any) bool {
	install, _ := mapValue(document, "installMapping")
	icon, _ := mapValue(install, "icon")
	return !boolValue(icon, "missing") && stringValue(icon, "sha256") != ""
}

func (s *Service) replaceInspectedIcon(ctx context.Context, releaseID string, analysis inspect.Analysis) {
	_ = s.DB.Queries.DeleteReleaseArtifactsByRole(ctx, sqlcdb.DeleteReleaseArtifactsByRoleParams{
		ReleaseID: releaseID,
		Role:      "icon",
	})
	if analysis.Icon == nil || len(analysis.Icon.Contents) == 0 {
		return
	}
	icon, err := s.Artifacts.Put(ctx, filepath.Base(analysis.Icon.SourcePath), "icon",
		bytes.NewReader(analysis.Icon.Contents))
	if err != nil {
		return
	}
	_ = s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
		ReleaseID:  releaseID,
		ArtifactID: icon.ID,
		Role:       "icon",
	})
}

func (s *Service) writeReleaseIcon(ctx context.Context, row sqlcdb.Release, work string) error {
	release := releaseDocument(row)
	rel := recipeFromDocument(release)
	name := recipe.IconSourceName(rel)
	if name == "" {
		return nil
	}
	dest := filepath.Join(work, name)
	icon := rel.InstallMapping.Icon
	if icon.SHA256 != "" {
		record, err := s.DB.Queries.GetArtifactBySHA256(ctx, icon.SHA256)
		if err == nil {
			path, pathErr := s.Artifacts.Store.Path(record.Sha256)
			if pathErr != nil {
				return pathErr
			}
			return copyFile(path, dest)
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
	}
	artifacts, err := s.DB.Queries.ListReleaseArtifacts(ctx, row.ID)
	if err != nil {
		return err
	}
	for _, item := range artifacts {
		if item.Role != "icon" {
			continue
		}
		record, getErr := s.Artifacts.Get(ctx, item.ArtifactID)
		if getErr != nil {
			continue
		}
		if icon.SHA256 != "" && !strings.EqualFold(record.SHA256, icon.SHA256) {
			continue
		}
		path, pathErr := s.Artifacts.Store.Path(record.SHA256)
		if pathErr != nil {
			return pathErr
		}
		return copyFile(path, dest)
	}
	member, ok := inspect.NormalizedArchivePath(icon.SourcePath)
	if !ok || member == "" || !row.SourceArtifactID.Valid {
		return fmt.Errorf("%w: configured icon %s is not available for the build", ErrInvalid, name)
	}
	source, err := s.Artifacts.Get(ctx, row.SourceArtifactID.String)
	if err != nil {
		return err
	}
	sourcePath, err := s.Artifacts.Store.Path(source.SHA256)
	if err != nil {
		return err
	}
	data, err := inspect.ReadPayloadFile(sourcePath, member, 4<<20)
	if err != nil {
		return fmt.Errorf("%w: %s", ErrInvalid, err.Error())
	}
	if icon.SHA256 != "" && sha256Hex(data) != strings.ToLower(icon.SHA256) {
		return fmt.Errorf("%w: payload icon %s no longer matches its recorded SHA256", ErrInvalid, member)
	}
	if put, putErr := s.Artifacts.Put(ctx, name, "icon", bytes.NewReader(data)); putErr == nil {
		_ = s.replaceIconArtifact(ctx, row.ID, put.ID)
	}
	if err := os.MkdirAll(filepath.Dir(dest), 0o700); err != nil {
		return err
	}
	return os.WriteFile(dest, data, 0o600)
}

func (s *Service) prepareBuildIdentity(ctx context.Context, row sqlcdb.Release, pkgbuild, vars string) (sqlcdb.Release, string, string, error) {
	arts, err := s.DB.Queries.ListReleaseArtifacts(ctx, row.ID)
	if err != nil {
		return row, vars, pkgbuild, err
	}
	hasBuilt := false
	for _, art := range arts {
		if art.Role == "built_package" {
			hasBuilt = true
			break
		}
	}
	if hasBuilt {
		row.ArchPkgrel++
		var body map[string]any
		if err := json.Unmarshal([]byte(row.BodyJson), &body); err != nil {
			body = map[string]any{}
		}
		body["archPkgrel"] = int(row.ArchPkgrel)
		raw, err := json.Marshal(body)
		if err != nil {
			return row, vars, pkgbuild, err
		}
		updated, err := s.DB.Queries.UpdateReleasePkgrel(ctx, sqlcdb.UpdateReleasePkgrelParams{
			ArchPkgrel: row.ArchPkgrel,
			BodyJson:   string(raw),
			ModifiedAt: nowUTC(),
			ID:         row.ID,
		})
		if err != nil {
			return row, vars, pkgbuild, err
		}
		row = updated
	}

	rel := recipeFromDocument(releaseDocument(row))
	rel.ArchPkgrel = int(row.ArchPkgrel)
	if s.Repo != nil {
		prep, err := s.Repo.PrepareBuild(ctx, row.ProjectID, row.ID)
		if err != nil {
			return row, vars, pkgbuild, err
		}
		if prep.PackageName != "" {
			rel.ArchPackageName = prep.PackageName
		}
		existingProvides := rel.PackageMetadata.Provides
		if prep.Publish && prep.OriginalName != "" && prep.OriginalName != prep.PackageName {
			if !containsToken(existingProvides, prep.OriginalName) {
				rel.CompatPackageName = prep.OriginalName
			}
		}
		rel.PackageMetadata.Provides = existingProvides
	}
	vars = recipe.IdentityVariables(rel)
	if !boolValue(releaseDocument(row).Document, "pkgbuildManuallyModified") {
		pkgbuild = recipe.Generate(rel)
	}
	return row, vars, pkgbuild, nil
}

func containsToken(values []string, name string) bool {
	name = strings.TrimSpace(name)
	for _, value := range values {
		token := strings.TrimSpace(value)
		for _, sep := range []string{"=", "<", ">", ":"} {
			if i := strings.IndexAny(token, sep); i > 0 {
				token = token[:i]
			}
		}
		if token == name {
			return true
		}
	}
	return false
}

func (s *Service) Cleanup(ctx context.Context) error {
	if s.Repo != nil {
		return s.Repo.CleanupExclusive(ctx, func(protected map[string]struct{}) error {
			return s.cleanupWith(ctx, protected)
		})
	}
	return s.cleanupWith(ctx, map[string]struct{}{})
}

func (s *Service) cleanupWith(ctx context.Context, protected map[string]struct{}) error {
	settings, err := s.DB.Queries.GetLibrarySettings(ctx)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return err
	}
	if err == nil && settings.RetentionVersions >= 0 {
		if err := s.pruneCompletedReleases(ctx, int(settings.RetentionVersions), protected); err != nil {
			return err
		}
	}

	roots := map[string]struct{}{}
	for id := range protected {
		roots[id] = struct{}{}
	}
	sources, err := s.DB.Queries.ListSourceArtifactIDs(ctx)
	if err != nil {
		return err
	}
	for _, id := range sources {
		if id.Valid {
			roots[id.String] = struct{}{}
		}
	}
	icons, err := s.DB.Queries.ListProjectIconArtifactIDs(ctx)
	if err != nil {
		return err
	}
	for _, id := range icons {
		if id.Valid {
			roots[id.String] = struct{}{}
		}
	}
	linked, err := s.DB.Queries.ListAllReleaseArtifactIDs(ctx)
	if err != nil {
		return err
	}
	for _, id := range linked {
		roots[id] = struct{}{}
	}
	artifacts, err := s.DB.Queries.ListArtifacts(ctx)
	if err != nil {
		return err
	}
	for _, art := range artifacts {
		if _, ok := roots[art.ID]; ok {
			continue
		}
		_ = s.Artifacts.Delete(ctx, art.ID)
	}
	return nil
}

func (s *Service) pruneCompletedReleases(ctx context.Context, keepOutdated int,
	protected map[string]struct{}) error {
	projects, err := s.DB.Queries.ListProjects(ctx)
	if err != nil {
		return err
	}
	channelEntries, err := s.DB.Queries.ListChannelEntries(ctx)
	if err != nil {
		return err
	}
	repoSettings, err := s.DB.Queries.GetRepoSettings(ctx)
	if err != nil {
		return err
	}
	for _, project := range projects {
		releases, err := s.DB.Queries.ListReleasesForProject(ctx, project.ID)
		if err != nil {
			return err
		}
		boundary := len(releases) - 1
		releaseIndexes := make(map[string]int, len(releases))
		for index, rel := range releases {
			releaseIndexes[rel.ID] = index
		}
		for _, entry := range channelEntries {
			if !entry.ProjectID.Valid || entry.ProjectID.String != project.ID ||
				!entry.ReleaseID.Valid || (entry.Channel == repo.ChannelStable && repoSettings.StableEnabled == 0) {
				continue
			}
			if index, ok := releaseIndexes[entry.ReleaseID.String]; ok && index < boundary {
				boundary = index
			}
		}
		completedSeen := 0
		for index := boundary - 1; index >= 0; index-- {
			rel := releases[index]
			arts, err := s.DB.Queries.ListReleaseArtifacts(ctx, rel.ID)
			if err != nil {
				return err
			}
			hasBuilt := false
			for _, art := range arts {
				if art.Role == "built_package" {
					hasBuilt = true
					break
				}
			}
			if !hasBuilt {
				continue
			}
			completedSeen++
			if completedSeen <= keepOutdated {
				continue
			}
			blocked := false
			for _, art := range arts {
				if _, ok := protected[art.ArtifactID]; ok {
					blocked = true
					break
				}
			}
			if rel.SourceArtifactID.Valid {
				if _, ok := protected[rel.SourceArtifactID.String]; ok {
					blocked = true
				}
			}
			if blocked {
				continue
			}
			if err := s.DB.Queries.DeleteRelease(ctx, rel.ID); err != nil {
				return err
			}
		}
	}
	return nil
}

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func projectFromRow(row sqlcdb.Project) Project {
	return Project{
		ID:                   row.ID,
		Revision:             row.Revision,
		DisplayName:          row.DisplayName,
		ArchPackageName:      row.ArchPackageName,
		VendorName:           row.VendorName,
		SourceIdentity:       row.SourceIdentity,
		IconSha256:           row.IconSha256,
		History:              decodeHistory(row.HistoryJson),
		CreatedAt:            row.CreatedAt,
		ModifiedAt:           row.ModifiedAt,
		RepoPublish:          row.RepoPublish != 0,
		RepoPkgnameOverride:  row.RepoPkgnameOverride,
		RepoPublishedPkgname: row.RepoPublishedPkgname,
		AutoBuildPolicy:      row.AutoBuildPolicy,
		CompileCachePolicy:   row.CompileCachePolicy,
	}
}

func releaseSummary(row sqlcdb.Release) Release {
	return Release{
		ID:              row.ID,
		ProjectID:       row.ProjectID,
		Revision:        row.Revision,
		State:           row.State,
		SourceType:      row.SourceType,
		VendorVersion:   row.VendorVersion,
		ArchPackageName: row.ArchPackageName,
		SourceSHA256:    row.SourceSha256,
		CreatedAt:       row.CreatedAt,
		ModifiedAt:      row.ModifiedAt,
		Document: map[string]any{
			"originalSourceFilename": row.OriginalFilename,
			"state":                  row.State,
			"archPkgrel":             row.ArchPkgrel,
			"debian": map[string]any{
				"version": row.VendorVersion,
			},
		},
	}
}

func releaseDocument(row sqlcdb.Release) Release {
	doc := map[string]any{}
	_ = json.Unmarshal([]byte(row.BodyJson), &doc)
	doc["id"] = row.ID
	doc["projectId"] = row.ProjectID
	doc["revision"] = row.Revision
	doc["state"] = row.State
	doc["sourceType"] = row.SourceType
	doc["sourceSha256"] = row.SourceSha256
	doc["archPackageName"] = row.ArchPackageName
	if stringValue(doc, "originalSourceFilename") == "" && row.OriginalFilename != "" {
		doc["originalSourceFilename"] = row.OriginalFilename
	}
	doc["createdAt"] = row.CreatedAt
	doc["modifiedAt"] = row.ModifiedAt
	return Release{
		ID:              row.ID,
		ProjectID:       row.ProjectID,
		Revision:        row.Revision,
		State:           row.State,
		SourceType:      row.SourceType,
		VendorVersion:   row.VendorVersion,
		ArchPackageName: row.ArchPackageName,
		SourceSHA256:    row.SourceSha256,
		CreatedAt:       row.CreatedAt,
		ModifiedAt:      row.ModifiedAt,
		Document:        doc,
	}
}

func attachIdentityVariables(row sqlcdb.Release, document map[string]any) {
	if document == nil {
		return
	}
	if stringValue(document, "originalSourceFilename") == "" && row.OriginalFilename != "" {
		document["originalSourceFilename"] = row.OriginalFilename
	}
	document["identityVariables"] = identityVariablesFor(Release{
		ID:              row.ID,
		ProjectID:       row.ProjectID,
		ArchPackageName: row.ArchPackageName,
		SourceType:      row.SourceType,
		SourceSHA256:    row.SourceSha256,
		VendorVersion:   row.VendorVersion,
		Document:        document,
	})
}

func stringValue(document map[string]any, key string) string {
	if document == nil {
		return ""
	}
	value, _ := document[key].(string)
	return value
}

func boolValue(document map[string]any, key string) bool {
	if document == nil {
		return false
	}
	value, ok := document[key].(bool)
	return ok && value
}

func boolValueDefault(document map[string]any, key string, fallback bool) bool {
	if document == nil {
		return fallback
	}
	if _, ok := document[key]; !ok {
		return fallback
	}
	return boolValue(document, key)
}

func mapValue(document map[string]any, key string) (map[string]any, bool) {
	if document == nil {
		return nil, false
	}
	value, ok := document[key].(map[string]any)
	return value, ok
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o700); err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return out.Close()
}

func (s *Service) persistSigningKeys(ctx context.Context, releaseID string, document map[string]any,
	extracted []inspect.ExtractedSigningKey, trusted []byte, trustedSource string) error {
	if document == nil {
		return nil
	}
	update, ok := mapValue(document, "update")
	if !ok {
		update = map[string]any{}
		document["update"] = update
	}
	var prepared []storedSigningKey
	seen := map[string]struct{}{}
	appendKey := func(key storedSigningKey) {
		if key.SHA256 == "" {
			return
		}
		if _, exists := seen[key.SHA256]; exists {
			return
		}
		seen[key.SHA256] = struct{}{}
		prepared = append(prepared, key)
	}
	if len(trusted) > 0 {
		if key, err := prepareSigningKey(trusted, firstNonEmpty(trustedSource, "repository-first import"),
			"", inspect.OriginUser); err == nil {
			appendKey(key)
		}
	}
	for _, key := range storedSigningKeysFromDocument(update) {
		if len(key.Contents) == 0 && key.ArtifactID == "" && key.SHA256 != "" {
			continue
		}
		if key.SHA256 == "" && len(key.Contents) > 0 {
			sum := sha256.Sum256(key.Contents)
			key.SHA256 = hex.EncodeToString(sum[:])
		}
		appendKey(key)
	}
	for _, key := range signingKeysFromExtracted(extracted) {
		appendKey(key)
	}
	encoded := make([]map[string]any, 0, len(prepared))
	for i, key := range prepared {
		if key.ArtifactID == "" && len(key.Contents) > 0 {
			record, err := s.Artifacts.Put(ctx, filepath.Base(key.RelativePath), "signing_key",
				bytes.NewReader(key.Contents))
			if err == nil {
				key.ArtifactID = record.ID
				if releaseID != "" {
					_ = s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
						ReleaseID:  releaseID,
						ArtifactID: record.ID,
						Role:       "signing_key",
					})
				}
			}
		}
		key.Contents = nil
		prepared[i] = key
		encoded = append(encoded, signingKeyJSON(key))
	}
	if len(encoded) > 0 {
		update["signingKeys"] = encoded
		if stringValue(update, "aptSigningKeyring") == "" {
			update["aptSigningKeyring"] = prepared[0].RelativePath
		}
		if stringValue(update, "trustedSigningFingerprint") == "" && len(prepared[0].Fingerprints) > 0 {
			update["trustedSigningFingerprint"] = prepared[0].Fingerprints[0]
		}
	}
	return nil
}

func (s *Service) associateSigningKeyArtifacts(ctx context.Context, releaseID string, document map[string]any) error {
	update, ok := mapValue(document, "update")
	if !ok {
		return nil
	}
	for _, key := range storedSigningKeysFromDocument(update) {
		if key.ArtifactID == "" {
			continue
		}
		_ = s.DB.Queries.InsertReleaseArtifact(ctx, sqlcdb.InsertReleaseArtifactParams{
			ReleaseID:  releaseID,
			ArtifactID: key.ArtifactID,
			Role:       "signing_key",
		})
	}
	return nil
}

func (s *Service) newestPreparedDocument(ctx context.Context, projectID, skipID string) map[string]any {
	rows, err := s.DB.Queries.ListReleasesForProject(ctx, projectID)
	if err != nil {
		return nil
	}
	for i := len(rows) - 1; i >= 0; i-- {
		row := rows[i]
		if row.ID == skipID || row.State == "discovered" || row.State == "preparing" {
			continue
		}
		return releaseDocument(row).Document
	}
	return nil
}

func (s *Service) nextPkgrel(ctx context.Context, projectID, version string) int {
	rows, err := s.DB.Queries.ListReleasesForProject(ctx, projectID)
	if err != nil {
		return 1
	}
	max := 0
	for _, row := range rows {
		if row.VendorVersion != version {
			continue
		}
		if int(row.ArchPkgrel) > max {
			max = int(row.ArchPkgrel)
		}
	}
	if max == 0 {
		return 1
	}
	return max + 1
}

func decodeTrustedKey(encoded string) ([]byte, error) {
	encoded = strings.TrimSpace(encoded)
	if encoded == "" {
		return nil, nil
	}
	decoded, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return nil, err
	}
	return decoded, nil
}
