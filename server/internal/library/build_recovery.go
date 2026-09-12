package library

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
)

func (s *Service) RecoverInterruptedBuilds(ctx context.Context, readLog func(string) (string, error)) error {
	tx, err := s.DB.SQL.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	queries := s.DB.Queries.WithTx(tx)
	releases, err := queries.ListStaleBuildingReleases(ctx)
	if err != nil {
		return err
	}
	for _, release := range releases {
		job, err := queries.GetLatestBuildJobForRelease(ctx, nullString(release.ID))
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		builds, err := queries.ListBuildsForRelease(ctx, release.ID)
		if err != nil {
			return err
		}
		status, logText := "failed", ""
		var completed *sqlcdb.Build
		if len(builds) > 0 {
			latest := &builds[len(builds)-1]
			finished, _ := time.Parse(time.RFC3339Nano, latest.FinishedAt.String)
			requested, _ := time.Parse(time.RFC3339Nano, job.CreatedAt)
			if job.StartedAt.Valid {
				requested, _ = time.Parse(time.RFC3339Nano, job.StartedAt.String)
			}
			// A crash after recording the build must not overwrite its completed result.
			if job.ID == "" || (!finished.IsZero() && !finished.Before(requested)) {
				completed = latest
			}
		}
		if completed != nil {
			status, logText = completed.Status, completed.LogText
		} else {
			id := "recovered-release-" + release.ID
			started, finished := sql.NullString{}, nullString(nowUTC())
			if job.ID != "" {
				id = "recovered-job-" + job.ID
				started = job.StartedAt
				if job.FinishedAt.Valid {
					finished = job.FinishedAt
				}
				logText, err = readLog(job.ID)
				if err != nil {
					return err
				}
			} else {
				var body map[string]any
				if err := json.Unmarshal([]byte(release.BodyJson), &body); err != nil {
					return err
				}
				logText, _ = body["lastBuildLog"].(string)
			}
			logText += "\n[PacSmith] Recovered an unfinished build after daemon restart. No worker owns this build; start a new build to retry.\n"
			if err := queries.InsertRecoveredBuild(ctx, sqlcdb.InsertRecoveredBuildParams{
				ID: id, ReleaseID: release.ID, Status: status, LogText: logText,
				StartedAt: started, FinishedAt: finished,
			}); err != nil {
				return err
			}
		}
		if err := queries.RecoverReleaseBuildState(ctx, sqlcdb.RecoverReleaseBuildStateParams{
			ID: release.ID, Status: status, LogText: logText, ModifiedAt: nowUTC(),
		}); err != nil {
			return err
		}
	}
	return tx.Commit()
}
