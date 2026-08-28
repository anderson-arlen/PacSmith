package daemon

import (
	"context"
	"database/sql"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
)

func (d *Daemon) startUpdateScheduler(parent context.Context) {
	ctx, cancel := context.WithCancel(parent)
	d.stopUpdates = cancel
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		d.enqueueScheduledUpdateIfDue(ctx, time.Now())
		for {
			select {
			case <-ctx.Done():
				return
			case now := <-ticker.C:
				d.enqueueScheduledUpdateIfDue(ctx, now)
			}
		}
	}()
}

func (d *Daemon) enqueueScheduledUpdateIfDue(ctx context.Context, now time.Time) {
	settings, err := d.db.Queries.GetLibrarySettings(ctx)
	if err != nil || settings.UpdatesEnabled == 0 {
		return
	}
	occurrence := scheduledOccurrence(now, settings.UpdatesDaily != 0,
		int(settings.UpdatesWeekday), int(settings.UpdatesHour), int(settings.UpdatesMinute))
	if occurrence.After(now) {
		return
	}
	var latest sql.NullString
	err = d.db.SQL.QueryRowContext(ctx,
		`SELECT MAX(created_at) FROM jobs WHERE kind = ? AND release_id IS NULL`,
		jobs.KindUpdateCheck).Scan(&latest)
	if err != nil {
		return
	}
	if latest.Valid {
		if checked, parseErr := time.Parse(time.RFC3339Nano, latest.String); parseErr == nil && !checked.Before(occurrence) {
			return
		}
	}
	payload := struct {
		Scheduled bool `json:"scheduled"`
	}{Scheduled: true}
	_, _ = d.jobs.Enqueue(ctx, jobs.KindUpdateCheck, payload, "", "")
}

func scheduledOccurrence(now time.Time, daily bool, weekday, hour, minute int) time.Time {
	location := now.Location()
	candidate := time.Date(now.Year(), now.Month(), now.Day(), hour, minute, 0, 0, location)
	if daily {
		if candidate.After(now) {
			candidate = candidate.AddDate(0, 0, -1)
		}
		return candidate
	}
	if weekday < 1 || weekday > 7 {
		weekday = 1
	}
	current := int(now.Weekday())
	if current == 0 {
		current = 7
	}
	delta := current - weekday
	if delta < 0 {
		delta += 7
	}
	candidate = candidate.AddDate(0, 0, -delta)
	if candidate.After(now) {
		candidate = candidate.AddDate(0, 0, -7)
	}
	return candidate
}
