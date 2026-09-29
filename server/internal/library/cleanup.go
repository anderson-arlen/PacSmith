package library

import (
	"context"
	"database/sql"
	"errors"

	"github.com/anderson-arlen/pacsmith/server/internal/repo"
)

func (s *Service) Cleanup(ctx context.Context) error {
	if s.Repo != nil {
		return s.Repo.CleanupExclusive(ctx, func(protected map[string]struct{}) error {
			return s.cleanupWith(ctx, protected)
		})
	}
	return s.cleanupWith(ctx, map[string]struct{}{})
}

func (s *Service) cleanupWith(ctx context.Context, protected map[string]struct{}) error {
	if err := s.trimProjectHistories(ctx); err != nil {
		return err
	}

	settings, err := s.DB.Queries.GetLibrarySettings(ctx)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return err
	}
	if err == nil && settings.RetentionVersions >= 0 {
		if err := s.pruneCompletedReleases(ctx, int(settings.RetentionVersions), protected); err != nil {
			return err
		}
	}

	return s.collectStorage(ctx, nil)
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
			if err := s.deleteReleaseStorage(ctx, rel.ID); err != nil {
				if errors.Is(err, ErrConflict) {
					continue
				}
				return err
			}
		}
	}
	return nil
}
