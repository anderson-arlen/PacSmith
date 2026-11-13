package library

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

func (s *Service) BuildRelease(ctx context.Context, releaseID string,
	logOutput func(string), automatic bool) (result BuildResult, buildErr error) {
	if _, err := s.GetRelease(ctx, releaseID); err != nil {
		return result, err
	}
	buildID, started := uuid.NewString(), nowUTC()
	defer func() {
		if buildErr == nil || result.Status == "succeeded" {
			return
		}
		// Cancellation and setup failures still need a durable result for this exact release.
		finishCtx := context.WithoutCancel(ctx)
		result.Status = "failed"
		result.Log += "\n[PacSmith] " + buildErr.Error() + "\n"
		if logOutput != nil {
			logOutput("\n[PacSmith] " + buildErr.Error() + "\n")
		}
		if err := s.DB.Queries.InsertRecoveredBuild(finishCtx, sqlcdb.InsertRecoveredBuildParams{
			ID: buildID, ReleaseID: releaseID, Status: "failed", LogText: result.Log,
			StartedAt: nullString(started), FinishedAt: nullString(nowUTC()),
		}); err != nil {
			buildErr = errors.Join(buildErr, err)
		}
		if err := s.DB.Queries.FinishBuild(finishCtx, sqlcdb.FinishBuildParams{
			Status: "failed", LogText: result.Log, FinishedAt: nullString(nowUTC()),
			ID: buildID, ReleaseID: releaseID,
		}); err != nil {
			buildErr = errors.Join(buildErr, err)
		}
		if err := s.recordBuildSummary(finishCtx, releaseID, "failed", result.Log, nil, automatic); err != nil {
			buildErr = errors.Join(buildErr, err)
		}
	}()
	if err := s.recordBuildSummary(ctx, releaseID, "building", "", nil, automatic); err != nil {
		return result, err
	}
	return s.buildRelease(ctx, releaseID, buildID, logOutput, automatic)
}

func (s *Service) buildRelease(ctx context.Context, releaseID, buildID string,
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
	result := BuildResult{Log: logText}
	now := nowUTC()
	build, buildErr := s.DB.Queries.InsertBuild(ctx, sqlcdb.InsertBuildParams{
		ID:         buildID,
		ReleaseID:  releaseID,
		Status:     status,
		LogText:    logText,
		StartedAt:  nullString(now),
		FinishedAt: nullString(now),
	})
	if buildErr != nil {
		return result, buildErr
	}
	if status != "succeeded" {
		if summaryErr := s.recordBuildSummary(ctx, releaseID, status, logText, nil, automatic); summaryErr != nil {
			return result, fmt.Errorf("makepkg failed; record build summary: %w", summaryErr)
		}
		return result, fmt.Errorf("makepkg failed")
	}
	entries := builtPackagePaths(work, link, source.SHA256)
	for _, path := range entries {
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
	result.Status = status
	if s.Repo != nil && len(result.Artifacts) > 0 {
		if err := s.Repo.PublishBuild(ctx, row.ProjectID, releaseID, result.Artifacts); err != nil {
			result.Log += "\nrepository publish: " + err.Error() + "\n"
			return result, err
		}
	}
	return result, nil
}
