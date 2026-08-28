package library

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"

	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func buildArtifactDocument(artifact sqlcdb.Artifact) map[string]any {
	result := map[string]any{
		"id":           artifact.ID,
		"relativePath": artifact.OriginalFilename,
		"sha256":       artifact.Sha256,
		"size":         artifact.SizeBytes,
		"createdAt":    artifact.CreatedAt,
	}
	if info, err := repo.ParseFilename(artifact.OriginalFilename); err == nil {
		result["packageName"] = info.Name
		result["packageVersion"] = info.Pkgver + "-" + info.Pkgrel
		result["architecture"] = info.Arch
	}
	return result
}

func (s *Service) attachBuildRecords(ctx context.Context, release *Release,
	builds []sqlcdb.Build) error {
	records := make([]map[string]any, 0, len(builds))
	latestArtifactCount := 0
	latestProducedPackages := []string{}
	for _, build := range builds {
		artifacts, err := s.DB.Queries.ListBuildArtifactsForBuild(ctx, build.ID)
		if err != nil {
			return err
		}
		artifactValues := make([]map[string]any, 0, len(artifacts))
		for _, item := range artifacts {
			artifactValues = append(artifactValues, buildArtifactDocument(item))
		}
		latestArtifactCount = len(artifactValues)
		latestProducedPackages = latestProducedPackages[:0]
		for _, item := range artifacts {
			latestProducedPackages = append(latestProducedPackages, item.OriginalFilename)
		}
		records = append(records, map[string]any{
			"id":         build.ID,
			"status":     build.Status,
			"log":        build.LogText,
			"artifacts":  artifactValues,
			"startedAt":  build.StartedAt.String,
			"finishedAt": build.FinishedAt.String,
		})
	}
	release.Document["builds"] = records
	if len(builds) == 0 {
		return nil
	}
	latest := builds[len(builds)-1]
	release.Document["buildStatus"] = latest.Status
	release.Document["lastBuildLog"] = latest.LogText
	release.Document["producedPackages"] = latestProducedPackages
	if latest.Status == "succeeded" && latestArtifactCount > 0 {
		release.State = "built"
		release.Document["state"] = "built"
	}
	return nil
}

func (s *Service) recordBuildSummary(ctx context.Context, releaseID, status, logText string,
	producedPackages []string, automatic bool) error {
	for attempt := 0; attempt < 3; attempt++ {
		row, err := s.DB.Queries.GetRelease(ctx, releaseID)
		if err != nil {
			return err
		}
		body := map[string]any{}
		if err := json.Unmarshal([]byte(row.BodyJson), &body); err != nil {
			return err
		}
		body["buildStatus"] = status
		body["automaticBuild"] = automatic
		body["lastBuildLog"] = logText
		body["producedPackages"] = producedPackages
		state := row.State
		if status == "succeeded" {
			state = "built"
			body["state"] = state
		}
		raw, err := json.Marshal(body)
		if err != nil {
			return err
		}
		_, err = s.DB.Queries.UpdateRelease(ctx, sqlcdb.UpdateReleaseParams{
			State: state, SourceType: row.SourceType, VendorVersion: row.VendorVersion,
			OriginalFilename: row.OriginalFilename, SourceSha256: row.SourceSha256,
			SourceArtifactID: row.SourceArtifactID, ArchPackageName: row.ArchPackageName,
			ArchPkgrel: row.ArchPkgrel, BodyJson: string(raw), ModifiedAt: nowUTC(),
			ID: row.ID, Revision: row.Revision,
		})
		if err == nil {
			return nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
	}
	return ErrConflict
}
