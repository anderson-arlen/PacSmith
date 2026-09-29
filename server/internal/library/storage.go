package library

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func (s *Service) DeleteRelease(ctx context.Context, id string) error {
	return s.deleteReleaseStorage(ctx, id)
}

func (s *Service) deleteReleaseStorage(ctx context.Context, id string) error {
	tx, err := s.DB.SQL.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	queries := s.DB.Queries.WithTx(tx)
	release, err := queries.GetRelease(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return err
	}
	if err := storageIdle(ctx, queries, release.ProjectID, id); err != nil {
		return err
	}
	artifacts, err := queries.ListReleaseStorageArtifactIDs(ctx, id)
	if err != nil {
		return err
	}
	if err := queries.DeleteRelease(ctx, id); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	return s.collectStorage(ctx, artifacts)
}

func (s *Service) DeleteProject(ctx context.Context, id string) error {
	tx, err := s.DB.SQL.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	queries := s.DB.Queries.WithTx(tx)
	project, err := queries.GetProject(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return err
	}
	if err := storageIdle(ctx, queries, id, ""); err != nil {
		return err
	}
	releases, err := queries.ListReleasesForProject(ctx, id)
	if err != nil {
		return err
	}
	var artifacts []sql.NullString
	if project.IconArtifactID.Valid {
		artifacts = append(artifacts, project.IconArtifactID)
	}
	for _, release := range releases {
		if err := storageIdle(ctx, queries, id, release.ID); err != nil {
			return err
		}
		ids, err := queries.ListReleaseStorageArtifactIDs(ctx, release.ID)
		if err != nil {
			return err
		}
		artifacts = append(artifacts, ids...)
	}
	if err := queries.DeleteProject(ctx, id); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	var repoErr error
	if s.Repo != nil {
		repoErr = s.Repo.OnProjectDeleted(ctx, id)
	}
	return errors.Join(repoErr, s.collectStorage(ctx, artifacts))
}

func storageIdle(ctx context.Context, queries *sqlcdb.Queries, projectID, releaseID string) error {
	count, err := queries.CountActiveStorageJobs(ctx, sqlcdb.CountActiveStorageJobsParams{
		ProjectID: nullString(projectID), ReleaseID: nullString(releaseID),
	})
	if err != nil {
		return err
	}
	if count > 0 {
		return fmt.Errorf("%w: wait for active project jobs to finish before deleting", ErrConflict)
	}
	return nil
}

// CollectStorage removes leftovers without applying the version retention policy.
func (s *Service) CollectStorage(ctx context.Context) error {
	return s.collectStorage(ctx, nil)
}

func (s *Service) collectStorage(ctx context.Context, deletedArtifacts []sql.NullString) error {
	// Finished rollout records must not pin packages after their release is deleted.
	if err := s.DB.Queries.DeleteFinishedOrphanSoaks(ctx); err != nil {
		return err
	}
	immediate := make(map[string]bool, len(deletedArtifacts))
	for _, id := range deletedArtifacts {
		immediate[id.String] = true
	}
	artifacts, err := s.DB.Queries.ListUnreferencedArtifacts(ctx)
	if err != nil {
		return err
	}
	var errs []error
	cutoff := time.Now().Add(-24 * time.Hour)
	for _, item := range artifacts {
		created, err := time.Parse(time.RFC3339Nano, item.CreatedAt)
		// Uploads are initially unreferenced while a separate import request is pending.
		if !immediate[item.ID] && (err != nil || !created.Before(cutoff)) {
			continue
		}
		if err := s.Artifacts.DeleteUnused(ctx, item.ID); err != nil {
			errs = append(errs, fmt.Errorf("remove artifact %s: %w", item.ID, err))
		}
	}
	errs = append(errs, s.cleanOrphanWorkspaces(ctx))
	return errors.Join(errs...)
}

func (s *Service) cleanOrphanWorkspaces(ctx context.Context) error {
	if s.WorkDir == "" {
		return nil
	}
	var errs []error
	for _, subdir := range []string{"releases", "cache/ccache", "cache/sources"} {
		root := filepath.Join(s.WorkDir, subdir)
		entries, err := os.ReadDir(root)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			errs = append(errs, err)
			continue
		}
		for _, entry := range entries {
			id := entry.Name()
			if subdir == "releases" {
				id = strings.TrimSuffix(id, ".preserved-src")
				_, err = s.DB.Queries.GetRelease(ctx, id)
			} else {
				_, err = s.DB.Queries.GetProject(ctx, id)
			}
			if err == nil {
				continue
			}
			if !errors.Is(err, sql.ErrNoRows) {
				return errors.Join(append(errs, err)...)
			}
			if err := storageIdle(ctx, s.DB.Queries, id, id); err != nil {
				if errors.Is(err, ErrConflict) {
					continue
				}
				return errors.Join(append(errs, err)...)
			}
			if err := os.RemoveAll(filepath.Join(root, entry.Name())); err != nil {
				errs = append(errs, fmt.Errorf("remove workspace %s: %w", entry.Name(), err))
			}
		}
	}
	return errors.Join(errs...)
}
