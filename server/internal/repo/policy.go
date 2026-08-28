package repo

import (
	"context"
	"database/sql"
	"errors"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

type successfulBuild struct {
	ReleaseID   string
	ArtifactIDs []string
}

type projectRepoPolicy struct {
	AutomaticSoak       bool
	SoakSecondsOverride int64
}

func (s *Service) projectPolicy(ctx context.Context, projectID string) (projectRepoPolicy, error) {
	var automatic, soakSecondsOverride int64
	err := s.DB.SQL.QueryRowContext(ctx, `
SELECT automatic_soak, soak_seconds_override
FROM project_repo_policies
WHERE project_id = ?`, projectID).Scan(&automatic, &soakSecondsOverride)
	if errors.Is(err, sql.ErrNoRows) {
		return projectRepoPolicy{SoakSecondsOverride: -1}, nil
	}
	return projectRepoPolicy{
		AutomaticSoak:       automatic != 0,
		SoakSecondsOverride: soakSecondsOverride,
	}, err
}

func (s *Service) saveProjectPolicy(ctx context.Context, projectID string,
	policy projectRepoPolicy) error {
	_, err := s.DB.SQL.ExecContext(ctx, `
INSERT INTO project_repo_policies (
    project_id, stable_enabled, automatic_soak, soak_seconds_override
)
VALUES (?, ?, ?, ?)
ON CONFLICT (project_id) DO UPDATE SET
    stable_enabled = excluded.stable_enabled,
    automatic_soak = excluded.automatic_soak,
    soak_seconds_override = excluded.soak_seconds_override`,
		projectID, 1, boolInt(policy.AutomaticSoak),
		policy.SoakSecondsOverride)
	return err
}

func effectiveSoakSeconds(libraryDefault int64, policy projectRepoPolicy) int64 {
	if policy.SoakSecondsOverride >= 0 {
		return policy.SoakSecondsOverride
	}
	return libraryDefault
}

func (s *Service) rescheduleProjectSoaks(ctx context.Context, projectID string,
	soakSeconds int64) error {
	soaks, err := s.DB.Queries.ListSoaks(ctx)
	if err != nil {
		return err
	}
	for _, soak := range soaks {
		if !soak.ProjectID.Valid || soak.ProjectID.String != projectID ||
			(soak.Status != SoakSoaking && soak.Status != SoakEligible) {
			continue
		}
		started, err := parseTime(soak.SoakStartedAt)
		if err != nil {
			continue
		}
		eligible := started.Add(time.Duration(soakSeconds) * time.Second)
		status := SoakSoaking
		if !eligible.After(s.now()) {
			status = SoakEligible
		}
		if err := s.DB.Queries.UpsertSoak(ctx, sqlcdb.UpsertSoakParams{
			Pkgname: soak.Pkgname, Arch: soak.Arch, Pkgver: soak.Pkgver,
			ProjectID: soak.ProjectID, ReleaseID: soak.ReleaseID, Epoch: soak.Epoch,
			Pkgrel: soak.Pkgrel, ArtifactID: soak.ArtifactID,
			SigArtifactID: soak.SigArtifactID, SoakStartedAt: soak.SoakStartedAt,
			EligibleAt: eligible.Truncate(time.Millisecond).Format("2006-01-02T15:04:05.000Z07:00"),
			Status:     status,
		}); err != nil {
			return err
		}
	}
	return nil
}

func (s *Service) rescheduleInheritedSoaks(ctx context.Context, soakSeconds int64) error {
	projects, err := s.DB.Queries.ListProjects(ctx)
	if err != nil {
		return err
	}
	for _, project := range projects {
		policy, err := s.projectPolicy(ctx, project.ID)
		if err != nil {
			return err
		}
		if !policy.AutomaticSoak || policy.SoakSecondsOverride >= 0 {
			continue
		}
		if err := s.rescheduleProjectSoaks(ctx, project.ID, soakSeconds); err != nil {
			return err
		}
	}
	return nil
}

func (s *Service) latestSuccessfulBuild(ctx context.Context,
	projectID string) (*successfulBuild, error) {
	releases, err := s.DB.Queries.ListReleasesForProject(ctx, projectID)
	if err != nil {
		return nil, err
	}
	for releaseIndex := len(releases) - 1; releaseIndex >= 0; releaseIndex-- {
		builds, err := s.DB.Queries.ListBuildsForRelease(ctx, releases[releaseIndex].ID)
		if err != nil {
			return nil, err
		}
		for buildIndex := len(builds) - 1; buildIndex >= 0; buildIndex-- {
			if builds[buildIndex].Status != "succeeded" {
				continue
			}
			artifacts, err := s.DB.Queries.ListBuildArtifactsForBuild(ctx, builds[buildIndex].ID)
			if err != nil {
				return nil, err
			}
			ids := make([]string, 0, len(artifacts))
			for _, artifact := range artifacts {
				ids = append(ids, artifact.ID)
			}
			if len(ids) > 0 {
				return &successfulBuild{ReleaseID: releases[releaseIndex].ID, ArtifactIDs: ids}, nil
			}
		}
	}
	return nil, nil
}
