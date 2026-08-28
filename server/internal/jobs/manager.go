package jobs

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite/sqlcdb"
	"github.com/google/uuid"
)

const (
	KindImport                 = "import"
	KindGitHubImport           = "github_import"
	KindRemoteImport           = "remote_import"
	KindRepositoryImport       = "repository_import"
	KindBuild                  = "build"
	KindUpdateCheck            = "update_check"
	KindUpdatePrepare          = "update_prepare"
	KindReanalyze              = "reanalyze"
	KindRepositoryDistribution = "repository_distribution"
)

var ErrNotFound = errors.New("job not found")

type Job struct {
	ID              string          `json:"id"`
	Kind            string          `json:"kind"`
	Status          string          `json:"status"`
	ProjectID       string          `json:"project_id,omitempty"`
	ProjectName     string          `json:"project_name,omitempty"`
	PackageName     string          `json:"package_name,omitempty"`
	ReleaseID       string          `json:"release_id,omitempty"`
	Error           string          `json:"error,omitempty"`
	LogOffset       int64           `json:"log_offset"`
	CreatedAt       string          `json:"created_at"`
	StartedAt       string          `json:"started_at,omitempty"`
	FinishedAt      string          `json:"finished_at,omitempty"`
	Result          json.RawMessage `json:"result,omitempty"`
	Message         string          `json:"message,omitempty"`
	Current         int64           `json:"current,omitempty"`
	Total           int64           `json:"total,omitempty"`
	FailedItems     int64           `json:"failed_items,omitempty"`
	PausedItems     int64           `json:"paused_items,omitempty"`
	ProjectAttached bool            `json:"-"`
}

type Progress struct {
	Message     string
	ProjectID   string
	ReleaseID   string
	ProjectName string
	PackageName string
	Current     int64
	Total       int64
	FailedItems int64
	PausedItems int64
}

type Handler func(ctx context.Context, job Job, payload json.RawMessage, log func(string),
	progress func(Progress)) (json.RawMessage, error)

type Manager struct {
	DB            *sqlite.DB
	LogDir        string
	Handle        Handler
	queue         chan string
	cancel        context.CancelFunc
	wg            sync.WaitGroup
	mu            sync.Mutex
	results       map[string]json.RawMessage
	canceled      map[string]struct{}
	currentID     string
	currentCancel context.CancelFunc
	observer      func(Job)
}

func New(db *sqlite.DB, logDir string, handle Handler) (*Manager, error) {
	if err := os.MkdirAll(logDir, 0o700); err != nil {
		return nil, err
	}
	if err := os.Chmod(logDir, 0o700); err != nil {
		return nil, err
	}
	return &Manager{
		DB:       db,
		LogDir:   logDir,
		Handle:   handle,
		queue:    make(chan string, 64),
		results:  map[string]json.RawMessage{},
		canceled: map[string]struct{}{},
	}, nil
}

func (m *Manager) Start(ctx context.Context) error {
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if err := m.DB.Queries.InterruptRunningJobs(ctx, sql.NullString{String: now, Valid: true}); err != nil {
		return err
	}
	runCtx, cancel := context.WithCancel(ctx)
	m.cancel = cancel
	m.wg.Add(1)
	go m.loop(runCtx)
	return nil
}

func (m *Manager) Stop() {
	if m.cancel != nil {
		m.cancel()
	}
	m.wg.Wait()
}

func (m *Manager) Enqueue(ctx context.Context, kind string, payload any, projectID, releaseID string) (Job, error) {
	if kind == KindUpdateCheck || kind == KindUpdatePrepare || kind == KindRepositoryDistribution {
		active, err := m.DB.Queries.ListActiveJobsByKind(ctx, kind)
		if err != nil {
			return Job{}, err
		}
		for _, existing := range active {
			if kind == KindUpdateCheck && existing.ReleaseID.String == releaseID {
				return jobFromRow(existing), nil
			}
			if kind == KindUpdatePrepare && existing.ReleaseID.String == releaseID {
				return jobFromRow(existing), nil
			}
			if kind == KindRepositoryDistribution && existing.ProjectID.String == projectID && projectID != "" {
				return jobFromRow(existing), nil
			}
		}
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		return Job{}, err
	}
	if raw == nil {
		raw = []byte("{}")
	}
	row, err := m.DB.Queries.InsertJob(ctx, sqlcdb.InsertJobParams{
		ID:          uuid.NewString(),
		Kind:        kind,
		Status:      "queued",
		ProjectID:   nullString(projectID),
		ReleaseID:   nullString(releaseID),
		PayloadJson: string(raw),
		CreatedAt:   time.Now().UTC().Format(time.RFC3339Nano),
	})
	if err != nil {
		return Job{}, err
	}
	select {
	case m.queue <- row.ID:
	case <-ctx.Done():
		return Job{}, ctx.Err()
	}
	job := jobFromRow(row)
	m.notify(job)
	return job, nil
}

func (m *Manager) SetObserver(observer func(Job)) {
	m.mu.Lock()
	m.observer = observer
	m.mu.Unlock()
}

func (m *Manager) notify(job Job) {
	m.mu.Lock()
	observer := m.observer
	m.mu.Unlock()
	if observer != nil {
		observer(job)
	}
}

func (m *Manager) Cancel(id string) error {
	if _, err := m.Get(context.Background(), id); err != nil {
		return err
	}
	m.mu.Lock()
	m.canceled[id] = struct{}{}
	cancel := m.currentCancel
	current := m.currentID
	m.mu.Unlock()
	if current == id && cancel != nil {
		cancel()
	}
	return nil
}

func (m *Manager) Get(ctx context.Context, id string) (Job, error) {
	row, err := m.DB.Queries.GetJob(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return Job{}, ErrNotFound
	}
	if err != nil {
		return Job{}, err
	}
	job := jobFromRow(row)
	m.mu.Lock()
	job.Result = m.results[id]
	m.mu.Unlock()
	return job, nil
}

func (m *Manager) Active(ctx context.Context, kind string) ([]Job, error) {
	rows, err := m.DB.Queries.ListActiveJobsByKind(ctx, kind)
	if err != nil {
		return nil, err
	}
	result := make([]Job, 0, len(rows))
	for _, row := range rows {
		result = append(result, jobFromRow(row))
	}
	return result, nil
}

func (m *Manager) Log(id string, after int64) (string, int64, error) {
	if after < 0 {
		after = 0
	}
	body, err := os.ReadFile(m.logPath(id))
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return "", 0, nil
		}
		return "", 0, err
	}
	if after >= int64(len(body)) {
		return "", int64(len(body)), nil
	}
	return string(body[after:]), int64(len(body)), nil
}

func (m *Manager) loop(ctx context.Context) {
	defer m.wg.Done()
	for {
		select {
		case <-ctx.Done():
			return
		case id := <-m.queue:
			m.run(ctx, id)
		}
	}
}

func (m *Manager) run(ctx context.Context, id string) {
	row, err := m.DB.Queries.GetJob(ctx, id)
	if err != nil {
		return
	}
	m.mu.Lock()
	_, skipped := m.canceled[id]
	m.mu.Unlock()
	started := time.Now().UTC().Format(time.RFC3339Nano)
	if skipped {
		updated, updateErr := m.DB.Queries.UpdateJob(ctx, sqlcdb.UpdateJobParams{
			Status:     "interrupted",
			Error:      "canceled",
			LogOffset:  m.logSize(id),
			StartedAt:  sql.NullString{String: started, Valid: true},
			FinishedAt: sql.NullString{String: started, Valid: true},
			ProjectID:  row.ProjectID,
			ReleaseID:  row.ReleaseID,
			ID:         row.ID,
		})
		if updateErr == nil {
			m.notify(jobFromRow(updated))
		}
		return
	}
	row, err = m.DB.Queries.UpdateJob(ctx, sqlcdb.UpdateJobParams{
		Status:     "running",
		Error:      "",
		LogOffset:  0,
		StartedAt:  sql.NullString{String: started, Valid: true},
		FinishedAt: sql.NullString{},
		ProjectID:  row.ProjectID,
		ReleaseID:  row.ReleaseID,
		ID:         row.ID,
	})
	if err != nil {
		return
	}
	m.notify(jobFromRow(row))
	jobCtx, cancel := context.WithCancel(ctx)
	m.mu.Lock()
	m.currentID = id
	m.currentCancel = cancel
	m.mu.Unlock()
	defer func() {
		cancel()
		m.mu.Lock()
		if m.currentID == id {
			m.currentID = ""
			m.currentCancel = nil
		}
		m.mu.Unlock()
	}()
	logFn := func(text string) {
		_ = m.appendLog(id, text)
	}
	job := jobFromRow(row)
	lastProgress := Progress{}
	hasProgress := false
	progressFn := func(progress Progress) {
		lastProgress = progress
		hasProgress = true
		projectAttached := !row.ProjectID.Valid && progress.ProjectID != ""
		projectID := row.ProjectID
		releaseID := row.ReleaseID
		if progress.ProjectID != "" {
			projectID = nullString(progress.ProjectID)
		}
		if progress.ReleaseID != "" {
			releaseID = nullString(progress.ReleaseID)
		}
		updated, err := m.DB.Queries.UpdateJob(jobCtx, sqlcdb.UpdateJobParams{
			Status: "running", Error: "", LogOffset: m.logSize(id),
			StartedAt: row.StartedAt, FinishedAt: sql.NullString{},
			ProjectID: projectID, ReleaseID: releaseID,
			Message: progress.Message, Current: progress.Current, Total: progress.Total,
			FailedItems: progress.FailedItems, PausedItems: progress.PausedItems, ID: row.ID,
		})
		if err != nil {
			return
		}
		row = updated
		update := jobFromRow(updated)
		update.ProjectAttached = projectAttached
		update.ProjectName = progress.ProjectName
		update.PackageName = progress.PackageName
		m.notify(update)
	}
	var result json.RawMessage
	runErr := error(nil)
	if m.Handle != nil {
		result, runErr = m.Handle(jobCtx, job, json.RawMessage(row.PayloadJson), logFn, progressFn)
	}
	if result != nil {
		m.mu.Lock()
		m.results[id] = result
		m.mu.Unlock()
	}
	finished := time.Now().UTC().Format(time.RFC3339Nano)
	status := "succeeded"
	errText := ""
	if runErr != nil {
		status = "failed"
		errText = runErr.Error()
		if errors.Is(runErr, context.Canceled) || errors.Is(jobCtx.Err(), context.Canceled) {
			status = "interrupted"
			errText = "canceled"
		}
		logFn(errText + "\n")
	}
	offset := m.logSize(id)
	updated, updateErr := m.DB.Queries.UpdateJob(ctx, sqlcdb.UpdateJobParams{
		Status:      status,
		Error:       errText,
		LogOffset:   offset,
		StartedAt:   sql.NullString{String: started, Valid: true},
		FinishedAt:  sql.NullString{String: finished, Valid: true},
		ProjectID:   row.ProjectID,
		ReleaseID:   row.ReleaseID,
		Message:     lastProgress.Message,
		Current:     lastProgress.Current,
		Total:       lastProgress.Total,
		FailedItems: lastProgress.FailedItems,
		PausedItems: lastProgress.PausedItems,
		ID:          row.ID,
	})
	if updateErr == nil {
		finishedJob := jobFromRow(updated)
		if hasProgress {
			finishedJob.Message = lastProgress.Message
			finishedJob.Current = lastProgress.Current
			finishedJob.Total = lastProgress.Total
			finishedJob.FailedItems = lastProgress.FailedItems
			finishedJob.PausedItems = lastProgress.PausedItems
		}
		if runErr != nil {
			finishedJob.Message = errText
		}
		m.notify(finishedJob)
	}
}

func (m *Manager) appendLog(id, text string) error {
	if text == "" {
		return nil
	}
	path := m.logPath(id)
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return err
	}
	defer file.Close()
	_, err = file.WriteString(text)
	return err
}

func (m *Manager) logSize(id string) int64 {
	info, err := os.Stat(m.logPath(id))
	if err != nil {
		return 0
	}
	return info.Size()
}

func (m *Manager) logPath(id string) string {
	return filepath.Join(m.LogDir, id+".log")
}

func jobFromRow(row sqlcdb.Job) Job {
	return Job{
		ID:          row.ID,
		Kind:        row.Kind,
		Status:      row.Status,
		ProjectID:   row.ProjectID.String,
		ReleaseID:   row.ReleaseID.String,
		Error:       row.Error,
		LogOffset:   row.LogOffset,
		CreatedAt:   row.CreatedAt,
		StartedAt:   row.StartedAt.String,
		FinishedAt:  row.FinishedAt.String,
		Message:     row.Message,
		Current:     row.Current,
		Total:       row.Total,
		FailedItems: row.FailedItems,
		PausedItems: row.PausedItems,
	}
}

func nullString(value string) sql.NullString {
	if value == "" {
		return sql.NullString{}
	}
	return sql.NullString{String: value, Valid: true}
}

func Wait(ctx context.Context, get func(context.Context, string) (Job, error), id string) (Job, error) {
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		job, err := get(ctx, id)
		if err != nil {
			return Job{}, err
		}
		switch job.Status {
		case "succeeded", "failed", "interrupted":
			return job, nil
		}
		select {
		case <-ctx.Done():
			return Job{}, ctx.Err()
		case <-ticker.C:
		}
	}
}

func Format(err error) string {
	if err == nil {
		return ""
	}
	return fmt.Sprintf("%v", err)
}
