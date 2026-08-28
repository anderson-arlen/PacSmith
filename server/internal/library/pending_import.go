package library

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"path/filepath"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/recipe"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

type PendingImportRequest struct {
	JobID             string
	ExistingProjectID string
	Version           string
	Filename          string
	CanonicalIdentity string
	SourceURL         string
	Acquisition       json.RawMessage
	ContentLength     int64
	Received          int64
}

func (s *Service) BeginPendingImport(ctx context.Context,
	request PendingImportRequest) (ImportResult, error) {
	if strings.TrimSpace(request.JobID) == "" || strings.TrimSpace(request.Filename) == "" {
		return ImportResult{}, fmt.Errorf("%w: pending import identity is incomplete", ErrInvalid)
	}
	var project sqlcdb.Project
	created := false
	var err error
	if request.ExistingProjectID != "" {
		project, err = s.DB.Queries.GetProject(ctx, request.ExistingProjectID)
		if err != nil {
			if err == sql.ErrNoRows {
				return ImportResult{}, fmt.Errorf("%w: project", ErrNotFound)
			}
			return ImportResult{}, err
		}
	} else {
		matches, matchErr := s.DB.Queries.ListProjectsBySourceIdentity(ctx, request.CanonicalIdentity)
		if matchErr != nil {
			return ImportResult{}, matchErr
		}
		if len(matches) > 0 {
			project = matches[0]
		} else {
			created = true
			displayName, packageName := pendingImportNames(request.Filename, request.Version)
			now := nowUTC()
			project, err = s.DB.Queries.InsertProject(ctx, sqlcdb.InsertProjectParams{
				ID: uuid.NewString(), DisplayName: displayName, ArchPackageName: packageName,
				SourceIdentity: request.CanonicalIdentity, IconArtifactID: sql.NullString{},
				HistoryJson: "[]", CreatedAt: now, ModifiedAt: now,
			})
			if err != nil {
				return ImportResult{}, err
			}
		}
	}

	releaseID := uuid.NewString()
	acquisition := map[string]any{"kind": "direct-url", "originalUrl": request.SourceURL,
		"canonicalIdentity": request.CanonicalIdentity}
	if len(request.Acquisition) > 0 {
		_ = json.Unmarshal(request.Acquisition, &acquisition)
	}
	body := map[string]any{
		"formatVersion": 1, "state": "preparing", "displayName": project.DisplayName,
		"archPackageName": project.ArchPackageName, "vendorName": project.VendorName,
		"originalSourceFilename": request.Filename, "sourceUrl": request.SourceURL,
		"debian": map[string]any{"package": project.ArchPackageName,
			"version": request.Version, "architecture": "x86_64"},
		"acquisition": acquisition,
		"importJob": map[string]any{"id": request.JobID, "status": "running",
			"bytesReceived": request.Received, "totalBytes": request.ContentLength,
			"projectCreated": created},
	}
	raw, err := json.Marshal(body)
	if err != nil {
		return ImportResult{}, err
	}
	now := nowUTC()
	_, err = s.DB.Queries.InsertRelease(ctx, sqlcdb.InsertReleaseParams{
		ID: releaseID, ProjectID: project.ID, State: "preparing", SourceType: "remote",
		VendorVersion: request.Version, OriginalFilename: request.Filename,
		SourceSha256: "pending:" + request.JobID, SourceArtifactID: sql.NullString{},
		ArchPackageName: project.ArchPackageName, ArchPkgrel: 1, BodyJson: string(raw),
		CreatedAt: now, ModifiedAt: now,
	})
	if err != nil {
		if created {
			_ = s.DB.Queries.DeleteProject(ctx, project.ID)
		}
		return ImportResult{}, err
	}
	return ImportResult{ProjectID: project.ID, ReleaseID: releaseID, ProjectCreated: created}, nil
}

func (s *Service) FinishPendingImport(ctx context.Context, releaseID, status, message string) error {
	row, err := s.DB.Queries.GetRelease(ctx, releaseID)
	if err != nil {
		return err
	}
	document := map[string]any{}
	_ = json.Unmarshal([]byte(row.BodyJson), &document)
	job, _ := document["importJob"].(map[string]any)
	if job == nil {
		job = map[string]any{}
	}
	job["status"] = status
	if message != "" {
		job["error"] = message
	}
	document["importJob"] = job
	document["state"] = "discovered"
	raw, err := json.Marshal(document)
	if err != nil {
		return err
	}
	_, err = s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
		State: "discovered", SourceType: row.SourceType, VendorVersion: row.VendorVersion,
		OriginalFilename: row.OriginalFilename, SourceSha256: row.SourceSha256,
		SourceArtifactID: row.SourceArtifactID, ArchPackageName: row.ArchPackageName,
		ArchPkgrel: row.ArchPkgrel, BodyJson: string(raw), ModifiedAt: nowUTC(),
		ID: row.ID, Revision: row.Revision,
	})
	return err
}

func (s *Service) RecoverInterruptedPendingImports(ctx context.Context) error {
	releases, err := s.DB.Queries.ListPreparingReleases(ctx)
	if err != nil {
		return err
	}
	for _, release := range releases {
		document := map[string]any{}
		if json.Unmarshal([]byte(release.BodyJson), &document) != nil {
			continue
		}
		job, _ := document["importJob"].(map[string]any)
		jobID, _ := job["id"].(string)
		if jobID == "" {
			continue
		}
		row, jobErr := s.DB.Queries.GetJob(ctx, jobID)
		if jobErr == nil && row.Status != "queued" && row.Status != "running" {
			message := row.Error
			if message == "" {
				message = "import interrupted before completion"
			}
			if err := s.FinishPendingImport(ctx, release.ID, row.Status, message); err != nil {
				return err
			}
		}
	}
	return nil
}

func pendingImportNames(filename, version string) (string, string) {
	name := filepath.Base(filename)
	for _, suffix := range []string{".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".appimage", ".deb", ".rpm", ".zip"} {
		if strings.HasSuffix(strings.ToLower(name), suffix) {
			name = name[:len(name)-len(suffix)]
			break
		}
	}
	name = strings.TrimSuffix(name, "-linux")
	if version != "" {
		name = strings.Trim(name, "-_.")
		name = strings.TrimSuffix(name, "-"+version)
	}
	packageName := recipe.SanitizePackageName(name)
	if packageName == "" {
		packageName = "vendor-package"
	}
	displayParts := strings.FieldsFunc(name, func(character rune) bool {
		return character == '-' || character == '_' || character == '.'
	})
	for index, part := range displayParts {
		if len(part) > 0 {
			displayParts[index] = strings.ToUpper(part[:1]) + part[1:]
		}
	}
	displayName := strings.Join(displayParts, " ")
	if displayName == "" {
		displayName = "Vendor Package"
	}
	return displayName, packageName + "-bin"
}
