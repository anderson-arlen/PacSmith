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
	"sync"
	"time"

	"github.com/anderson-arlen/pacsmith/server/internal/ai"
	"github.com/anderson-arlen/pacsmith/server/internal/artifact"
	"github.com/anderson-arlen/pacsmith/server/internal/auth"
	"github.com/anderson-arlen/pacsmith/server/internal/httpapi"
	"github.com/anderson-arlen/pacsmith/server/internal/jobs"
	"github.com/anderson-arlen/pacsmith/server/internal/library"
	"github.com/anderson-arlen/pacsmith/server/internal/listen"
	"github.com/anderson-arlen/pacsmith/server/internal/paths"
	"github.com/anderson-arlen/pacsmith/server/internal/pki"
	"github.com/anderson-arlen/pacsmith/server/internal/repo"
	"github.com/anderson-arlen/pacsmith/server/internal/secret"
	"github.com/anderson-arlen/pacsmith/server/internal/sqlite"
)

type Config struct {
	Dirs   paths.Dirs
	Listen string
}

type Daemon struct {
	Dirs       paths.Dirs
	server     *http.Server
	handler    http.Handler
	pki        *pki.Runtime
	listen     *listen.State
	tlsMu      sync.Mutex
	tlsServes  []tlsServe
	tlsAddr    string
	repo       *repo.Service
	repoMu     sync.Mutex
	repoListen listen.Config
	repoServes []repoServe
	stopSoak   context.CancelFunc
	db         *sqlite.DB
	jobs       *jobs.Manager
	closeOnce  sync.Once
	closeErr   error
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
	lib.Repo = repoSvc
	manager, err := jobs.New(db, filepath.Join(cfg.Dirs.Work, "jobs"), JobHandler(lib, opened.Store))
	if err != nil {
		_ = db.Close()
		return nil, err
	}
	if err := manager.Start(ctx); err != nil {
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
	return d, nil
}

func JobHandler(lib *library.Service, secrets *secret.LockedStore) jobs.Handler {
	aiService := &ai.Service{Secrets: secrets}
	return func(ctx context.Context, job jobs.Job, payload json.RawMessage, log func(string)) (json.RawMessage, error) {
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
			}
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			log("Running makepkg…\n")
			result, err := lib.BuildRelease(ctx, req.ReleaseID)
			if result.Log != "" {
				log(result.Log)
			}
			raw, marshalErr := json.Marshal(result)
			if err != nil {
				return raw, err
			}
			return raw, marshalErr
		case jobs.KindAi:
			var req struct {
				ReleaseID string `json:"release_id"`
			}
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			settings, err := libraryAISettings(ctx, lib)
			if err != nil {
				return nil, err
			}
			release, err := lib.GetRelease(ctx, req.ReleaseID)
			if err != nil {
				return nil, err
			}
			resolution := aiService.ResolvePackage(ctx, ai.PackageRequest{
				Settings: settings,
				Document: release.Document,
			}, log)
			raw, marshalErr := json.Marshal(resolution)
			if marshalErr != nil {
				return nil, marshalErr
			}
			return raw, resolution.Err()
		case jobs.KindAiGitHubAsset:
			var req struct {
				Owner      string   `json:"github_owner"`
				Repository string   `json:"github_repository"`
				Preferred  string   `json:"preferred_asset"`
				Assets     []string `json:"available_assets"`
			}
			if err := json.Unmarshal(payload, &req); err != nil {
				return nil, err
			}
			settings, err := libraryAISettings(ctx, lib)
			if err != nil {
				return nil, err
			}
			resolution := aiService.ResolveGitHubAsset(ctx, ai.GitHubAssetRequest{
				Settings:   settings,
				Owner:      req.Owner,
				Repository: req.Repository,
				Preferred:  req.Preferred,
				Assets:     req.Assets,
			}, log)
			raw, marshalErr := json.Marshal(resolution)
			if marshalErr != nil {
				return nil, marshalErr
			}
			return raw, resolution.Err()
		default:
			return nil, fmt.Errorf("unknown job kind %q", job.Kind)
		}
	}
}

func libraryAISettings(ctx context.Context, lib *library.Service) (ai.Settings, error) {
	row, err := lib.DB.Queries.GetLibrarySettings(ctx)
	if err != nil {
		return ai.Settings{}, err
	}
	return ai.SettingsFromStore(row.AiProvider, row.AiModel, row.AiReasoningEffort, row.AiExecutionMode), nil
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
