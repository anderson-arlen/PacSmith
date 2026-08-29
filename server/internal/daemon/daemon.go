package daemon

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/auth"
	"github.com/anderson-arlen/pacsmith/server/internal/events"
	githubapi "github.com/anderson-arlen/pacsmith/server/internal/github"
	"github.com/anderson-arlen/pacsmith/server/internal/httpapi"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
	"github.com/anderson-arlen/pacsmith/server/internal/paths"
	"github.com/anderson-arlen/pacsmith/server/internal/pki"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
	"github.com/anderson-arlen/pacsmith/server/internal/updatecheck"
)

type Config struct {
	Dirs   paths.Dirs
	Listen string
}

type Daemon struct {
	Dirs        paths.Dirs
	server      *http.Server
	handler     http.Handler
	pki         *pki.Runtime
	listen      *listen.State
	tlsMu       sync.Mutex
	tlsServes   []tlsServe
	tlsAddr     string
	repo        *repo.Service
	repoMu      sync.Mutex
	repoListen  listen.Config
	repoServes  []repoServe
	stopSoak    context.CancelFunc
	stopUpdates context.CancelFunc
	db          *sqlite.DB
	jobs        *jobs.Manager
	events      *events.Hub
	closeOnce   sync.Once
	closeErr    error
}

type tlsServe struct {
	addr     string
	listener net.Listener
	server   *http.Server
}

func Start(ctx context.Context, dirs paths.Dirs) (*Daemon, error) {
	return StartConfig(ctx, Config{Dirs: dirs})
}

func StartConfig(ctx context.Context, cfg Config) (*Daemon, error) {
	if err := cfg.Dirs.Ensure(); err != nil {
		return nil, err
	}
	db, err := sqlite.Open(ctx, cfg.Dirs.Database)
	if err != nil {
		return nil, err
	}
	opened, err := secret.Open(ctx, db, cfg.Dirs.Config)
	if err != nil {
		_ = db.Close()
		return nil, err
	}
	for _, name := range []string{"openai.api_key", "xai.api_key", "chatgpt.session"} {
		_ = opened.Store.Delete(ctx, name)
		_ = db.Queries.DeleteCredentialMeta(ctx, name)
	}
	runtime, err := pki.LoadOrGenerate(ctx, db, opened.Store)
	if err != nil {
		_ = db.Close()
		return nil, err
	}
	store, err := artifact.New(cfg.Dirs.Objects, cfg.Dirs.Tmp)
	if err != nil {
		_ = db.Close()
		return nil, err
	}
	registry := &artifact.Registry{DB: db, Store: store}
	lib := &library.Service{
		DB:        db,
		Artifacts: registry,
		WorkDir:   filepath.Join(cfg.Dirs.Work, "releases"),
	}
	repoSvc := repo.New(db, registry, opened.Store, filepath.Join(cfg.Dirs.Work, "repo"), filepath.Join(cfg.Dirs.Data, "gnupg"))
	eventHub := events.New()
	lib.Repo = repoSvc
	githubSvc := &githubapi.Service{Secrets: opened.Store, Artifacts: registry}
	updateSvc := &updatecheck.Service{DB: db, Library: lib, Artifacts: registry, GitHub: githubSvc}
	manager, err := jobs.New(db, filepath.Join(cfg.Dirs.Work, "jobs"), JobHandler(lib, githubSvc, updateSvc))
	if err != nil {
		_ = db.Close()
		return nil, err
	}
	if err := manager.Start(ctx); err != nil {
		_ = db.Close()
		return nil, err
	}
	if err := lib.RecoverInterruptedPendingImports(ctx); err != nil {
		manager.Stop()
		_ = db.Close()
		return nil, err
	}
	listenState := &listen.State{}
	stored, err := db.Queries.GetServerState(ctx)
	if err != nil {
		manager.Stop()
		_ = db.Close()
		return nil, err
	}
	listenCfg := listen.FromStore(stored.ListenEnabled != 0, int(stored.ListenPort), stored.ListenHosts)
	if cfg.Listen != "" {
		override, err := listen.ParseOverride(cfg.Listen)
		if err != nil {
			manager.Stop()
			_ = db.Close()
			return nil, err
		}
		listenCfg = override
	}
	listenState.Set(listenCfg)
	d := &Daemon{
		Dirs:   cfg.Dirs,
		pki:    runtime,
		listen: listenState,
		repo:   repoSvc,
		db:     db,
		jobs:   manager,
		events: eventHub,
	}
	handler := httpapi.New(httpapi.Config{
		DB:          db,
		Artifacts:   registry,
		Library:     lib,
		Jobs:        manager,
		Secrets:     opened.Store,
		PKI:         runtime,
		Principal:   auth.LocalUnix(),
		Listen:      listenState,
		ApplyListen: d.SetListen,
		Repo:        repoSvc,
		ApplyRepo:   d.SetRepoListen,
		RepoBound:   d.RepoBound,
		Events:      eventHub,
		GitHub:      githubSvc,
		Updates:     updateSvc,
	})
	d.handler = handler
	listener, err := listenUnix(cfg.Dirs.Socket)
	if err != nil {
		manager.Stop()
		_ = db.Close()
		return nil, err
	}
	server := httpapi.NewHTTPServer(handler)
	d.server = server
	go func() {
		if serveErr := server.Serve(listener); serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			_ = d.Close()
		}
	}()
	if listenCfg.Enabled {
		if err := d.SetListen(listenCfg); err != nil {
			_ = d.Close()
			return nil, err
		}
	}
	if err := waitReady(ctx, cfg.Dirs.Socket); err != nil {
		_ = d.Close()
		return nil, err
	}
	if settings, err := repoSvc.Settings(ctx); err == nil && settings.Enabled {
		if err := d.SetRepoListen(settings.ListenConfig()); err != nil {
			_ = d.Close()
			return nil, err
		}
	}
	d.startRepoMaintenance()
	d.startUpdateScheduler(ctx)
	return d, nil
}

func JobHandler(lib *library.Service, githubSvc *githubapi.Service,
	updaters ...*updatecheck.Service) jobs.Handler {
	var updater *updatecheck.Service
	if len(updaters) > 0 {
		updater = updaters[0]
	}
	return func(ctx context.Context, job jobs.Job, payload json.RawMessage, log func(string),
		progress func(jobs.Progress)) (json.RawMessage, error) {
		switch job.Kind {
		case jobs.KindImport:
			var req library.ImportRequest
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			log("Inspecting vendor artifact…\n")
			result, err := lib.ImportArtifact(ctx, req)
			if err != nil {
				return nil, err
			}
			log(fmt.Sprintf("Imported project %s release %s\n", result.ProjectID, result.ReleaseID))
			return json.Marshal(result)
		case jobs.KindGitHubImport:
			var req githubapi.ImportRequest
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			if githubSvc == nil {
				return nil, fmt.Errorf("GitHub service is unavailable")
			}
			log("Resolving GitHub release artifact…\n")
			source, err := githubSvc.Resolve(ctx, githubapi.ResolveRequest{
				URL: req.URL, AssetRegex: req.AssetRegex,
				IncludePrereleases: req.IncludePrereleases,
			})
			if err != nil {
				return nil, err
			}
			if source.DownloadURL == "" {
				message := source.Message
				if len(source.AvailableAssets) > 0 {
					message += ". Available artifacts: " + strings.Join(source.AvailableAssets, ", ")
				}
				return nil, errors.New(message)
			}
			log(fmt.Sprintf("Downloading %s…\n", source.Filename))
			record, err := githubSvc.Download(ctx, source)
			if err != nil {
				return nil, err
			}
			acquisition, err := json.Marshal(map[string]any{
				"kind": "github-release", "canonicalIdentity": "github:" + source.Owner + "/" + source.Repository,
				"originalUrl": source.DownloadURL, "githubOwner": source.Owner,
				"githubRepository": source.Repository, "githubReleaseId": source.ReleaseID,
				"githubAssetId": source.AssetID, "githubTag": source.Tag,
				"githubAssetName": source.Filename, "githubPrerelease": source.Prerelease,
				"publisherDigest": source.PublisherDigest,
			})
			if err != nil {
				return nil, err
			}
			log("Inspecting vendor artifact…\n")
			imported, err := lib.ImportArtifact(ctx, library.ImportRequest{
				ArtifactID: record.ID, ExistingProjectID: req.ExistingProjectID,
				Version: source.DetectedVersion, ExpectedSHA256: source.SHA256,
				AcquisitionKind:   "github-release",
				CanonicalIdentity: "github:" + source.Owner + "/" + source.Repository,
				Acquisition:       acquisition, GitHubAssetRegex: source.AssetRegex,
				GitHubIncludePrereleases: req.IncludePrereleases,
			})
			if err != nil {
				return nil, err
			}
			return json.Marshal(map[string]any{
				"project_id": imported.ProjectID, "release_id": imported.ReleaseID,
				"project_created": imported.ProjectCreated, "duplicate": imported.Duplicate,
				"source": source,
			})
		case jobs.KindRemoteImport:
			if updater == nil {
				return nil, fmt.Errorf("remote importer is unavailable")
			}
			var req updatecheck.DirectImportRequest
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			result, err := updater.ImportDirectURL(ctx, req, job.ID, log,
				func(update updatecheck.Progress) {
					progress(jobs.Progress{Message: update.Message, ProjectID: update.ProjectID,
						ReleaseID: update.ReleaseID, ProjectName: update.ProjectName,
						PackageName: update.PackageName, Current: update.Current, Total: update.Total})
				})
			raw, marshalErr := json.Marshal(result)
			if err != nil {
				return raw, err
			}
			return raw, marshalErr
		case jobs.KindRepositoryImport:
			if updater == nil {
				return nil, fmt.Errorf("repository importer is unavailable")
			}
			var req updatecheck.RepositoryImportRequest
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			result, err := updater.ImportRepository(ctx, req, log)
			raw, marshalErr := json.Marshal(result)
			if err != nil {
				return raw, err
			}
			return raw, marshalErr
		case jobs.KindReanalyze:
			var req struct {
				ReleaseID string `json:"release_id"`
			}
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			log("Reanalyzing stored artifact…\n")
			result, err := lib.Reanalyze(ctx, req.ReleaseID)
			if err != nil {
				return nil, err
			}
			return json.Marshal(result)
		case jobs.KindBuild:
			var req struct {
				ReleaseID string `json:"release_id"`
				Automatic string `json:"automatic"`
			}
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			log("Running makepkg…\n")
			result, err := lib.BuildRelease(ctx, req.ReleaseID, log, req.Automatic == "true")
			raw, marshalErr := json.Marshal(result)
			historyErr := lib.RecordBuildOutcome(context.WithoutCancel(ctx), req.ReleaseID,
				req.Automatic == "true", err)
			if err != nil {
				if historyErr != nil {
					log("Could not record build history: " + historyErr.Error() + "\n")
				}
				return raw, err
			}
			if historyErr != nil {
				return raw, fmt.Errorf("record build history: %w", historyErr)
			}
			return raw, marshalErr
		case jobs.KindUpdateCheck:
			if updater == nil {
				return nil, fmt.Errorf("update checker is unavailable")
			}
			var req struct {
				ReleaseID string `json:"release_id"`
				Force     bool   `json:"force"`
			}
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			result, err := updater.Run(ctx, req.ReleaseID, req.Force, log,
				func(update updatecheck.Progress) {
					progress(jobs.Progress{Message: update.Message, ProjectID: update.ProjectID,
						ReleaseID: update.ReleaseID, ProjectName: update.ProjectName,
						PackageName: update.PackageName, Current: update.Current, Total: update.Total})
				})
			raw, marshalErr := json.Marshal(result)
			if err != nil {
				if job.ProjectID != "" {
					if _, historyErr := lib.AppendProjectHistory(context.WithoutCancel(ctx),
						job.ProjectID, "update-check", "error: "+err.Error()); historyErr != nil {
						log("Could not record update-check history: " + historyErr.Error() + "\n")
					}
				}
				return raw, err
			}
			progress(jobs.Progress{Message: updateBatchSummary(result),
				Current: int64(len(result.Checks)), Total: int64(len(result.Checks)),
				FailedItems: int64(result.Failed), PausedItems: int64(updatePausedCount(result))})
			return raw, marshalErr
		case jobs.KindUpdatePrepare:
			if updater == nil {
				return nil, fmt.Errorf("update checker is unavailable")
			}
			var req struct {
				ReleaseID string `json:"release_id"`
			}
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			result, err := updater.PrepareDiscovered(ctx, req.ReleaseID, log,
				func(update updatecheck.Progress) {
					progress(jobs.Progress{Message: update.Message, ProjectID: update.ProjectID,
						ReleaseID: update.ReleaseID, ProjectName: update.ProjectName,
						PackageName: update.PackageName, Current: update.Current, Total: update.Total})
				})
			raw, marshalErr := json.Marshal(result)
			if err != nil {
				return raw, err
			}
			return raw, marshalErr
		case jobs.KindRepositoryDistribution:
			log("Reconciling repository distribution…\n")
			if err := lib.Repo.ReconcileProjectDistribution(ctx, job.ProjectID); err != nil {
				return nil, err
			}
			log("Repository distribution is up to date\n")
			return json.Marshal(map[string]string{"project_id": job.ProjectID})
		default:
			return nil, fmt.Errorf("unknown job kind %q", job.Kind)
		}
	}
}

func updateBatchSummary(result updatecheck.BatchResult) string {
	available := 0
	built := 0
	failures := make([]string, 0, result.Failed)
	paused := make([]string, 0)
	for _, check := range result.Checks {
		if check.UpdateAvailable {
			available++
		}
		if check.Built {
			built++
		}
		if check.Status == "error" {
			name := check.ProjectName
			if name == "" {
				name = check.PackageName
			}
			if name == "" {
				name = check.ProjectID
			}
			if name == "" {
				name = "Unknown package"
			}
			message := strings.TrimSpace(check.Message)
			if message == "" {
				message = "update check failed without an error message"
			}
			failures = append(failures, fmt.Sprintf("%s — %s", name, message))
		}
		if check.AutomaticStatus == "paused" {
			name := check.ProjectName
			if name == "" {
				name = check.PackageName
			}
			if name == "" {
				name = check.ProjectID
			}
			message := strings.TrimSpace(check.AutomaticMessage)
			if message == "" {
				message = "automatic handling paused without a reason"
			}
			paused = append(paused, fmt.Sprintf("%s — %s", name, message))
		}
	}
	if len(failures) > 0 || len(paused) > 0 {
		summary := fmt.Sprintf("Update check finished: %d update(s) found; %d built automatically",
			available, built)
		if len(failures) > 0 {
			summary += fmt.Sprintf("; %d check(s) failed.\nFailed checks:\n• %s",
				len(failures), strings.Join(failures, "\n• "))
		} else {
			summary += "."
		}
		if len(paused) > 0 {
			summary += fmt.Sprintf("\nAutomatic handling paused:\n• %s",
				strings.Join(paused, "\n• "))
		}
		return summary
	}
	if available > 0 {
		return fmt.Sprintf("Update check finished: %d update(s) found; %d built automatically",
			available, built)
	}
	if built > 0 {
		return fmt.Sprintf("Update check finished: all vendor versions are current; %d prepared update(s) built automatically",
			built)
	}
	return "Update check finished: all eligible packages are current"
}

func updatePausedCount(result updatecheck.BatchResult) int {
	paused := 0
	for _, check := range result.Checks {
		if check.AutomaticStatus == "paused" {
			paused++
		}
	}
	return paused
}

func (d *Daemon) Close() error {
	if d == nil {
		return nil
	}
	d.closeOnce.Do(func() {
		var errs []error
		if d.stopSoak != nil {
			d.stopSoak()
		}
		if d.stopUpdates != nil {
			d.stopUpdates()
		}
		d.repoMu.Lock()
		d.stopRepoLocked()
		d.repoMu.Unlock()
		if d.jobs != nil {
			d.jobs.Stop()
		}
		shutdown := func(server *http.Server) {
			if server == nil {
				return
			}
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			errs = append(errs, server.Shutdown(ctx))
			cancel()
		}
		shutdown(d.server)
		d.stopTLS()
		if d.db != nil {
			errs = append(errs, d.db.Close())
		}
		d.closeErr = errors.Join(errs...)
	})
	return d.closeErr
}

func (d *Daemon) TLSAddr() string {
	if d == nil {
		return ""
	}
	return d.tlsAddr
}

func listenUnix(socket string) (net.Listener, error) {
	if err := os.Remove(socket); err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("remove stale socket: %w", err)
	}
	listener, err := net.Listen("unix", socket)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", socket, err)
	}
	if err := os.Chmod(socket, 0o600); err != nil {
		_ = listener.Close()
		return nil, fmt.Errorf("chmod socket: %w", err)
	}
	return listener, nil
}

func waitReady(ctx context.Context, socket string) error {
	deadline := time.Now().Add(5 * time.Second)
	for {
		conn, err := net.DialTimeout("unix", socket, 50*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("pacsmithd did not become ready: %w", err)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(20 * time.Millisecond):
		}
	}
}
