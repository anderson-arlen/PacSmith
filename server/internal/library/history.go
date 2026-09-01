package library

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

const (
	retainedProjectHistoryEntries = 100
	historyCleanupAttempts        = 3
)

func (s *Service) AppendProjectHistory(ctx context.Context, projectID, event, detail string) (Project, error) {
	projectID = strings.TrimSpace(projectID)
	event = strings.TrimSpace(event)
	detail = strings.TrimSpace(detail)
	if projectID == "" || event == "" || detail == "" {
		return Project{}, fmt.Errorf("%w: project history entry is incomplete", ErrInvalid)
	}
	now := nowUTC()
	entry, err := json.Marshal(HistoryEntry{Timestamp: now, Event: event, Detail: detail})
	if err != nil {
		return Project{}, err
	}
	row, err := s.DB.Queries.AppendProjectHistory(ctx, sqlcdb.AppendProjectHistoryParams{
		EntryJson: string(entry), ModifiedAt: now, ID: projectID,
	})
	if errors.Is(err, sql.ErrNoRows) {
		if _, lookupErr := s.DB.Queries.GetProject(ctx, projectID); errors.Is(lookupErr, sql.ErrNoRows) {
			return Project{}, ErrNotFound
		} else if lookupErr != nil {
			return Project{}, lookupErr
		}
		return Project{}, fmt.Errorf("project %q has invalid history data", projectID)
	}
	if err != nil {
		return Project{}, err
	}
	return projectFromRow(row), nil
}

func (s *Service) trimProjectHistories(ctx context.Context) error {
	projects, err := s.DB.Queries.ListProjects(ctx)
	if err != nil {
		return err
	}
	for _, project := range projects {
		if err := s.trimProjectHistory(ctx, project); err != nil {
			return err
		}
	}
	return nil
}

func (s *Service) trimProjectHistory(ctx context.Context, project sqlcdb.Project) error {
	for attempt := 0; attempt < historyCleanupAttempts; attempt++ {
		history := decodeHistory(project.HistoryJson)
		if len(history) <= retainedProjectHistoryEntries {
			return nil
		}
		retained := history[len(history)-retainedProjectHistoryEntries:]
		_, err := s.DB.Queries.ReplaceProjectHistory(ctx, sqlcdb.ReplaceProjectHistoryParams{
			HistoryJson: encodeHistory(retained),
			ModifiedAt:  nowUTC(),
			ID:          project.ID,
			Revision:    project.Revision,
		})
		if err == nil {
			return nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		project, err = s.DB.Queries.GetProject(ctx, project.ID)
		if errors.Is(err, sql.ErrNoRows) {
			return nil
		}
		if err != nil {
			return err
		}
	}
	return fmt.Errorf("%w: project %q changed during history cleanup", ErrConflict, project.ID)
}

func (s *Service) recordImportedRelease(ctx context.Context, projectID, releaseID string) error {
	release, err := s.GetRelease(ctx, releaseID)
	if err != nil {
		return err
	}
	detail := "Imported release " + release.VendorVersion
	if filename := strings.TrimSpace(stringValue(release.Document, "originalSourceFilename")); filename != "" {
		detail += " from " + filename
	}
	_, err = s.AppendProjectHistory(ctx, projectID, "release-imported", detail)
	return err
}

func (s *Service) RecordBuildOutcome(ctx context.Context, releaseID string, automatic bool,
	buildErr error) error {
	release, err := s.GetRelease(ctx, releaseID)
	if err != nil {
		return err
	}
	detail := "Build of release " + release.VendorVersion
	if automatic {
		detail = "Automatic build of release " + release.VendorVersion
	}
	if buildErr == nil {
		detail += " succeeded"
	} else if errors.Is(buildErr, context.Canceled) {
		detail += " was canceled"
	} else {
		detail += " failed: " + buildErr.Error()
	}
	_, err = s.AppendProjectHistory(ctx, release.ProjectID, "build", detail)
	return err
}

func (s *Service) RecordPackageOperation(ctx context.Context, projectID, releaseID, operation string,
	exitCode int, canceled bool, failure string) (Project, error) {
	project, err := s.GetProject(ctx, projectID)
	if err != nil {
		return Project{}, err
	}
	operation = strings.TrimSpace(operation)
	label := ""
	switch operation {
	case "install":
		label = "Installation"
	case "rollback":
		label = "Rollback"
	case "uninstall":
		label = "Uninstall"
	default:
		return Project{}, fmt.Errorf("%w: unsupported package operation", ErrInvalid)
	}
	target := project.ArchPackageName
	if releaseID != "" {
		release, releaseErr := s.GetRelease(ctx, releaseID)
		if releaseErr != nil {
			return Project{}, releaseErr
		}
		if release.ProjectID != project.ID {
			return Project{}, fmt.Errorf("%w: release does not belong to project", ErrInvalid)
		}
		target = "release " + release.VendorVersion
	}
	detail := label + " of " + target
	failure = strings.Join(strings.Fields(failure), " ")
	failureRunes := []rune(failure)
	if len(failureRunes) > 1024 {
		failure = string(failureRunes[:1024])
	}
	if canceled {
		detail += " was canceled"
	} else if exitCode == 0 {
		detail += " succeeded"
	} else {
		detail += " failed with exit code " + strconv.Itoa(exitCode)
		if failure != "" {
			detail += ": " + failure
		}
	}
	return s.AppendProjectHistory(ctx, project.ID, operation, detail)
}
